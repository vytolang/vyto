/* volt/hw/usb native backing — USB device enumeration via Linux sysfs.

   The Volt-way hardware pattern (docs/HARDWARE.md): a thin C shim turns the
   device tree into an opaque handle the Volt side owns and frees in deinit, with
   fields/bytes crossing the boundary — no external library, no #link. Here the
   "device" is the whole bus: usb_enumerate() snapshots every device under
   /sys/bus/usb/devices into a UsbList, and the accessors read it out. Read-only
   and root-free (sysfs attributes are world-readable). Transfers (bulk/interrupt
   over /dev/bus/usb via USBDEVFS ioctls, or libusb) are a later step; this proves
   the enumeration + ownership pattern end to end.

   Non-Linux builds compile to an empty list (macOS would use IOKit; noted). */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int vid, pid, bus, addr, cls;
    char mfr[128], product[128], serial[128], speed[16];
} UsbDev;

typedef struct {
    UsbDev *v;
    int n, cap;
} UsbList;

#ifdef __linux__
#include <dirent.h>

/* Read a sysfs attribute file into buf (NUL-terminated, trailing newline
   stripped). Returns 1 on success, 0 if absent/empty. */
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

static int read_hex(const char *dir, const char *name) {
    char b[32];
    if (!read_attr(dir, name, b, sizeof b)) return -1;
    return (int)strtol(b, NULL, 16);
}
static int read_dec(const char *dir, const char *name) {
    char b[32];
    if (!read_attr(dir, name, b, sizeof b)) return -1;
    return (int)strtol(b, NULL, 10);
}

static void list_push(UsbList *l, const UsbDev *d) {
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 16;
        UsbDev *nv = (UsbDev *)realloc(l->v, (size_t)want * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = want;
    }
    l->v[l->n++] = *d;
}

UsbList *usb_enumerate(void) {
    UsbList *l = (UsbList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    const char *root = "/sys/bus/usb/devices";
    DIR *dp = opendir(root);
    if (!dp) return l; /* no USB subsystem — empty list, not an error */
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        if (strchr(de->d_name, ':')) continue; /* "1-3:1.0" is an interface, skip */
        char dir[512];
        snprintf(dir, sizeof dir, "%s/%s", root, de->d_name);
        int vid = read_hex(dir, "idVendor");
        if (vid < 0) continue; /* not a device node (e.g. a controller) */
        UsbDev d;
        memset(&d, 0, sizeof d);
        d.vid = vid;
        d.pid = read_hex(dir, "idProduct");
        d.cls = read_hex(dir, "bDeviceClass");
        d.bus = read_dec(dir, "busnum");
        d.addr = read_dec(dir, "devnum");
        read_attr(dir, "manufacturer", d.mfr, sizeof d.mfr);
        read_attr(dir, "product", d.product, sizeof d.product);
        read_attr(dir, "serial", d.serial, sizeof d.serial);
        read_attr(dir, "speed", d.speed, sizeof d.speed);
        list_push(l, &d);
    }
    closedir(dp);
    return l;
}

#else /* non-Linux: empty list (macOS would enumerate via IOKit) */

UsbList *usb_enumerate(void) { return (UsbList *)calloc(1, sizeof(UsbList)); }

#endif

int usb_count(UsbList *l) { return l ? l->n : 0; }

static UsbDev *at(UsbList *l, int i) {
    if (!l || i < 0 || i >= l->n) return NULL;
    return &l->v[i];
}
int usb_vid(UsbList *l, int i)  { UsbDev *d = at(l, i); return d ? d->vid : 0; }
int usb_pid(UsbList *l, int i)  { UsbDev *d = at(l, i); return d ? d->pid : 0; }
int usb_bus(UsbList *l, int i)  { UsbDev *d = at(l, i); return d ? d->bus : 0; }
int usb_addr(UsbList *l, int i) { UsbDev *d = at(l, i); return d ? d->addr : 0; }
int usb_class(UsbList *l, int i){ UsbDev *d = at(l, i); return d ? d->cls : 0; }
const char *usb_mfr(UsbList *l, int i)     { UsbDev *d = at(l, i); return d ? d->mfr : ""; }
const char *usb_product(UsbList *l, int i) { UsbDev *d = at(l, i); return d ? d->product : ""; }
const char *usb_serial(UsbList *l, int i)  { UsbDev *d = at(l, i); return d ? d->serial : ""; }
const char *usb_speed(UsbList *l, int i)   { UsbDev *d = at(l, i); return d ? d->speed : ""; }

void usb_free(UsbList *l) {
    if (!l) return;
    free(l->v);
    free(l);
}
