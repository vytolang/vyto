/* vyto/hw/sensors native backing — IIO sensors via Linux sysfs.

   The Vyto-way hardware pattern (docs/HARDWARE.md), the request/response half (the usb
   half): a thin sysfs shim turns the IIO subsystem into an opaque snapshot the Vyto
   side owns and frees, with samples crossing as bytes/strings. IIO (Industrial I/O) is
   the kernel's home for accelerometers, gyroscopes, magnetometers, light/proximity/
   temperature/pressure sensors, ADCs — anything that produces scalar channels.

   iio_enumerate() lists /sys/bus/iio/devices/iio:deviceN with each device's name.
   iio_channels() lists the readable channel bases (files matching in_*_raw, with the
   _raw suffix stripped). iio_read_attr() reads any attribute file so the Vyto side can
   compose in_<chan>_raw / _scale / _offset and apply the standard raw*scale+offset.

   Read-only, world-readable sysfs — zero #link, root-free. The buffered path
   (/dev/iio:deviceN with a trigger, an fd folded into a PollSet) is a later increment;
   this proves the sample-read half. Non-Linux compiles to an empty list. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int  index;             /* N in iio:deviceN */
    char name[128];
} IioDev;

typedef struct {
    IioDev *v;
    int n, cap;
} IioList;

#ifdef __linux__
#include <dirent.h>

/* Read a sysfs attribute file into buf (NUL-terminated, trailing whitespace stripped).
   Returns 1 on success, 0 if absent/empty. Same shape as usb_shim's read_attr. */
static int read_attr(const char *dir, const char *name, char *buf, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return 0; }
    size_t got = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[got] = 0;
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' || buf[got - 1] == ' '))
        buf[--got] = 0;
    return got > 0;
}

static void list_push(IioList *l, const IioDev *d) {
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 8;
        IioDev *nv = (IioDev *)realloc(l->v, (size_t)want * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = want;
    }
    l->v[l->n++] = *d;
}

IioList *iio_enumerate(void) {
    IioList *l = (IioList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    const char *root = "/sys/bus/iio/devices";
    DIR *dp = opendir(root);
    if (!dp) return l; /* no IIO subsystem — empty list, not an error */
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (strncmp(de->d_name, "iio:device", 10) != 0) continue;
        IioDev d;
        memset(&d, 0, sizeof d);
        d.index = (int)strtol(de->d_name + 10, NULL, 10);
        char dir[512];
        snprintf(dir, sizeof dir, "%s/%s", root, de->d_name);
        read_attr(dir, "name", d.name, sizeof d.name);
        list_push(l, &d);
    }
    closedir(dp);
    return l;
}

/* Directory of iio:device<index>. */
static void dev_dir(int index, char *out, size_t cap) {
    snprintf(out, cap, "/sys/bus/iio/devices/iio:device%d", index);
}

/* NUL-terminated, '\n'-joined list of channel bases: every file named in_*_raw with
   the "in_" prefix and "_raw" suffix removed (e.g. "accel_x", "temp"). The Vyto side
   splits it and rebuilds in_<base>_raw / _scale / _offset. Stored in a file-static
   buffer (single-threaded, same as the usb accessors). */
static char g_channels[4096];

const char *iio_channels(IioList *l, int i) {
    g_channels[0] = 0;
    if (!l || i < 0 || i >= l->n) return g_channels;
    char dir[512];
    dev_dir(l->v[i].index, dir, sizeof dir);
    DIR *dp = opendir(dir);
    if (!dp) return g_channels;
    struct dirent *de;
    size_t used = 0;
    while ((de = readdir(dp))) {
        const char *nm = de->d_name;
        size_t len = strlen(nm);
        if (strncmp(nm, "in_", 3) != 0) continue;
        if (len <= 7 || strcmp(nm + len - 4, "_raw") != 0) continue; /* want in_*_raw */
        size_t blen = len - 3 - 4; /* strip "in_" and "_raw" */
        if (used + blen + 2 >= sizeof g_channels) break;
        if (used) g_channels[used++] = '\n';
        memcpy(g_channels + used, nm + 3, blen);
        used += blen;
        g_channels[used] = 0;
    }
    closedir(dp);
    return g_channels;
}

/* Read an arbitrary attribute of iio:device<index> (e.g. "in_accel_x_raw",
   "in_accel_scale"). Empty string when absent. File-static return buffer. */
static char g_attr[128];

const char *iio_read_attr(int index, const char *attr) {
    char dir[512];
    dev_dir(index, dir, sizeof dir);
    if (!read_attr(dir, attr, g_attr, sizeof g_attr)) g_attr[0] = 0;
    return g_attr;
}

/* --- Buffered streaming (the fd half) -----------------------------------------

   The sample reads above are request/response; a hardware/software trigger plus a
   kernel buffer turns IIO into the streaming shape — /dev/iio:deviceN becomes a
   poll-able fd delivering packed scan records, folding into a PollSet like serial or
   evdev. Setup is sysfs (enable scan channels, assign a trigger, set buffer length,
   enable the buffer), all via iio_write_attr(); the fd then streams. Decoding the
   packed record is per-device (scan_elements layout / endianness), so the bytes cross
   as a raw byte[] — "own the samples", the doc's rule. */

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Write `val` to an attribute of iio:device<index> (e.g. "buffer/enable" -> "1",
   "scan_elements/in_accel_x_en" -> "1"). 0 ok, -1 on error. */
int iio_write_attr(int index, const char *attr, const char *val) {
    char path[512];
    snprintf(path, sizeof path, "/sys/bus/iio/devices/iio:device%d/%s", index, attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t w = write(fd, val, strlen(val));
    close(fd);
    return w < 0 ? -1 : 0;
}

/* Open /dev/iio:device<index> non-blocking for buffered reads. fd, or -1. */
int iio_buffer_open(int index) {
    char path[64];
    snprintf(path, sizeof path, "/dev/iio:device%d", index);
    return open(path, O_RDONLY | O_NONBLOCK);
}

/* >0 = bytes of packed scan data, 0 = none/EOF, -1 = would-block, -2 = error. Same
   encoded returns as serial/socket so a poll loop can tell "later" from a failure. */
long iio_buffer_read(int fd, char *buf, long cap) {
    long r = (long)read(fd, buf, (size_t)cap);
    if (r > 0) return r;
    if (r == 0) return 0;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

void iio_buffer_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: empty list, no attributes, no buffer */

IioList *iio_enumerate(void) { return (IioList *)calloc(1, sizeof(IioList)); }
const char *iio_channels(IioList *l, int i) { (void)l; (void)i; return ""; }
const char *iio_read_attr(int index, const char *attr) { (void)index; (void)attr; return ""; }
int  iio_write_attr(int index, const char *attr, const char *val) { (void)index; (void)attr; (void)val; return -1; }
int  iio_buffer_open(int index) { (void)index; return -1; }
long iio_buffer_read(int fd, char *buf, long cap) { (void)fd; (void)buf; (void)cap; return -2; }
void iio_buffer_close(int fd) { (void)fd; }

#endif

int iio_count(IioList *l) { return l ? l->n : 0; }

static IioDev *at(IioList *l, int i) {
    if (!l || i < 0 || i >= l->n) return NULL;
    return &l->v[i];
}
int         iio_index(IioList *l, int i) { IioDev *d = at(l, i); return d ? d->index : -1; }
const char *iio_name(IioList *l, int i)  { IioDev *d = at(l, i); return d ? d->name : ""; }

void iio_free(IioList *l) {
    if (!l) return;
    free(l->v);
    free(l);
}
