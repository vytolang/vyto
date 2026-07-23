/* vyto/hw/usb native backing — USB device enumeration via Linux sysfs.

   The Vyto-way hardware pattern (docs/HARDWARE.md): a thin C shim turns the
   device tree into an opaque handle the Vyto side owns and frees in deinit, with
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

/* Append a device to the growable list (shared by every platform's enumerator). */
static void list_push(UsbList *l, const UsbDev *d) {
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 16;
        UsbDev *nv = (UsbDev *)realloc(l->v, (size_t)want * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = want;
    }
    l->v[l->n++] = *d;
}

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

#elif defined(__APPLE__) && defined(VYTO_USB_IOKIT)
/* macOS IOKit enumeration.

   UNTESTED — written from the IOKit/CoreFoundation spec, never run (no macOS here).
   Gated behind -DVYTO_USB_IOKIT because it needs frameworks the Vyto #link path can't
   express (`-framework IOKit -framework CoreFoundation`, not `-l`): without the macro
   a macOS build keeps the working empty-list below, so this never breaks a mac build.
   To use: compile the package with
     -DVYTO_USB_IOKIT -framework IOKit -framework CoreFoundation
   and validate on real hardware before trusting it. */

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* Read an integer USB property (e.g. CFSTR("idVendor")) into an int, or -1. */
static int io_int(io_service_t dev, CFStringRef key) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0);
    if (!ref) return -1;
    int out = -1;
    if (CFGetTypeID(ref) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)ref, kCFNumberIntType, &out);
    CFRelease(ref);
    return out;
}

/* Read a string USB property into buf (empty on absence). */
static void io_str(io_service_t dev, CFStringRef key, char *buf, size_t cap) {
    buf[0] = 0;
    CFTypeRef ref = IORegistryEntryCreateCFProperty(dev, key, kCFAllocatorDefault, 0);
    if (!ref) return;
    if (CFGetTypeID(ref) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)ref, buf, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(ref);
}

UsbList *usb_enumerate(void) {
    UsbList *l = (UsbList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return l;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match, &it) != KERN_SUCCESS)
        return l;
    io_service_t dev;
    while ((dev = IOIteratorNext(it))) {
        UsbDev d;
        memset(&d, 0, sizeof d);
        d.vid = io_int(dev, CFSTR("idVendor"));
        d.pid = io_int(dev, CFSTR("idProduct"));
        d.cls = io_int(dev, CFSTR("bDeviceClass"));
        d.addr = io_int(dev, CFSTR("USB Address"));
        int loc = io_int(dev, CFSTR("locationID"));
        d.bus = loc >= 0 ? ((loc >> 24) & 0xFF) : -1;   /* top byte of locationID ~ bus */
        io_str(dev, CFSTR("USB Vendor Name"), d.mfr, sizeof d.mfr);
        io_str(dev, CFSTR("USB Product Name"), d.product, sizeof d.product);
        io_str(dev, CFSTR("USB Serial Number"), d.serial, sizeof d.serial);
        {
            int mbps = io_int(dev, CFSTR("Device Speed")); /* 0 low,1 full,2 high,3 super */
            const char *s = "";
            if (mbps == 0) s = "1.5"; else if (mbps == 1) s = "12";
            else if (mbps == 2) s = "480"; else if (mbps == 3) s = "5000";
            snprintf(d.speed, sizeof d.speed, "%s", s);
        }
        if (d.vid >= 0) list_push(l, &d);
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    return l;
}

#else /* other non-Linux (incl. default macOS): empty list */

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

/* --- Transfers: open a device node and move bytes over usbfs -----------------

   The doc's stated "next step" on the same pattern: an opaque per-device handle
   (here a plain fd into /dev/bus/usb/BBB/DDD) with a byte[] transfer call and a
   deinit that closes it, exactly like Socket. Bulk/interrupt transfers go through
   the usbfs USBDEVFS_* ioctls — no libusb, no #link. Opening a device node needs
   write access (usually a udev rule or root), so a denied open is soft (-1). */

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

/* Open /dev/bus/usb/<bus>/<addr> read-write. Returns the fd, or -1 (missing node or
   no permission). bus/addr are the busnum/devnum the enumeration already reports. */
int usb_open(int bus, int addr) {
    char path[64];
    snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, addr);
    return open(path, O_RDWR);
}

/* Claim/release an interface so the kernel lets us talk to its endpoints. 0 ok, -1. */
int usb_claim(int fd, int iface) {
    unsigned int n = (unsigned int)iface;
    return ioctl(fd, USBDEVFS_CLAIMINTERFACE, &n) == 0 ? 0 : -1;
}
int usb_release(int fd, int iface) {
    unsigned int n = (unsigned int)iface;
    return ioctl(fd, USBDEVFS_RELEASEINTERFACE, &n) == 0 ? 0 : -1;
}

/* One bulk/interrupt transfer on endpoint `ep` (direction is the ep's 0x80 bit).
   >=0 bytes transferred, -1 timeout / would-block, -2 error. */
long usb_bulk(int fd, int ep, char *buf, long len, int timeout_ms) {
    struct usbdevfs_bulktransfer bt;
    bt.ep = (unsigned int)ep;
    bt.len = (unsigned int)len;
    bt.timeout = (unsigned int)timeout_ms;
    bt.data = buf;
    int r = ioctl(fd, USBDEVFS_BULK, &bt);
    if (r >= 0) return (long)r;
    if (errno == ETIMEDOUT || errno == EAGAIN) return -1;
    return -2;
}

void usb_dev_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: transfers unavailable */

int  usb_open(int bus, int addr) { (void)bus; (void)addr; return -1; }
int  usb_claim(int fd, int iface) { (void)fd; (void)iface; return -1; }
int  usb_release(int fd, int iface) { (void)fd; (void)iface; return -1; }
long usb_bulk(int fd, int ep, char *buf, long len, int timeout_ms) {
    (void)fd; (void)ep; (void)buf; (void)len; (void)timeout_ms; return -2;
}
void usb_dev_close(int fd) { (void)fd; }

#endif
