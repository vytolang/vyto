/* vyto/hw/input native backing — input devices via Linux evdev.

   The Vyto-way streaming hardware pattern (docs/HARDWARE.md): an input device is a
   fd under /dev/input/event*, opened non-blocking so it folds into a PollSet event
   loop alongside sockets, windows, and serial ports — one wait() covers the network,
   the wire, AND the gamepad/keyboard/touch. Two halves live here:

     - Enumeration (the usb half): input_enumerate() snapshots every /dev/input/event*
       into an opaque InputList, reading each device's name/id, then closing it. The
       Vyto side copies the fields out and frees the handle.
     - Streaming (the serial half): evdev_open() hands back a non-blocking fd;
       evdev_next() reads exactly one input_event into a file-static struct and the
       accessors read its type/code/value out. Parsing the event in C keeps the
       boundary at plain ints — struct input_event's timeval layout is arch-dependent,
       so it never crosses as a raw byte[]. Single-threaded (Vyto has no threads), so
       the static is safe, same reasoning as the usb accessors.

   Zero #link, read-only enumeration; opening event nodes usually needs the `input`
   group (soft: absent/denied -> empty/-1). Non-Linux compiles to empty/-1 stubs. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    char path[64];
    char name[128];
    int  vendor, product;
} InputInfo;

typedef struct {
    InputInfo *v;
    int n, cap;
} InputList;

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static void list_push(InputList *l, const InputInfo *d) {
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 16;
        InputInfo *nv = (InputInfo *)realloc(l->v, (size_t)want * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = want;
    }
    l->v[l->n++] = *d;
}

InputList *input_enumerate(void) {
    InputList *l = (InputList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    const char *root = "/dev/input";
    DIR *dp = opendir(root);
    if (!dp) return l; /* no input subsystem — empty list, not an error */
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        InputInfo d;
        memset(&d, 0, sizeof d);
        snprintf(d.path, sizeof d.path, "%s/%s", root, de->d_name);
        int fd = open(d.path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue; /* no permission on this node — skip it */
        char nm[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof nm - 1), nm) >= 0) {
            strncpy(d.name, nm, sizeof d.name - 1);
        }
        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) >= 0) {
            d.vendor = id.vendor;
            d.product = id.product;
        }
        close(fd);
        list_push(l, &d);
    }
    closedir(dp);
    return l;
}

/* Open an event node non-blocking. Returns the fd, or -1 (missing / no permission). */
int evdev_open(const char *path) {
    return open(path, O_RDONLY | O_NONBLOCK);
}

/* The last event read by evdev_next (single-threaded, so a file-static is safe). */
static struct input_event g_ev;

/* Read exactly one input_event. 1 = got one (accessors valid), 0 = would-block / EOF,
   -1 = error. A partial read (never expected on a chardev) counts as would-block. */
int evdev_next(int fd) {
    ssize_t r = read(fd, &g_ev, sizeof g_ev);
    if (r == (ssize_t)sizeof g_ev) return 1;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (r == 0) return 0; /* EOF (device unplugged) */
    return (r < 0) ? -1 : 0;
}

int evdev_type(void)  { return (int)g_ev.type; }
int evdev_code(void)  { return (int)g_ev.code; }
int evdev_value(void) { return (int)g_ev.value; }

void evdev_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: empty list, no device access */

InputList *input_enumerate(void) { return (InputList *)calloc(1, sizeof(InputList)); }
int  evdev_open(const char *path) { (void)path; return -1; }
int  evdev_next(int fd) { (void)fd; return -1; }
int  evdev_type(void)  { return 0; }
int  evdev_code(void)  { return 0; }
int  evdev_value(void) { return 0; }
void evdev_close(int fd) { (void)fd; }

#endif

int input_count(InputList *l) { return l ? l->n : 0; }

static InputInfo *at(InputList *l, int i) {
    if (!l || i < 0 || i >= l->n) return NULL;
    return &l->v[i];
}
const char *input_path(InputList *l, int i)   { InputInfo *d = at(l, i); return d ? d->path : ""; }
const char *input_name(InputList *l, int i)   { InputInfo *d = at(l, i); return d ? d->name : ""; }
int         input_vendor(InputList *l, int i)  { InputInfo *d = at(l, i); return d ? d->vendor : 0; }
int         input_product(InputList *l, int i) { InputInfo *d = at(l, i); return d ? d->product : 0; }

void input_free(InputList *l) {
    if (!l) return;
    free(l->v);
    free(l);
}
