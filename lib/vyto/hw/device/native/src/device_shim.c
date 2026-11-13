/* vyto/hw/device native backing — generic device byte I/O, and HID listing.

   The lowest common denominator of the Vyto-way (docs/HARDWARE.md): an open
   device with read/write/seek, non-blocking so it folds into a PollSet like
   every other streaming device. This reaches ANY device whose contract is
   read()/write(): hidraw, tty, /dev/random, /dev/cec*, pipes; on Windows a
   HID interface path, "\\.\" device names, named pipes. Control calls go
   through vyto/hw/ioctl with rawFd(). Same encoded returns as serial/socket.

   The "fd" handed to Vyto is a handle id: the descriptor on POSIX, a table
   slot on Windows. dev_raw() is what vyto/hw/ioctl takes (the descriptor, or
   the HANDLE's value), and dev_poll_fd() what a PollSet takes.

   Windows: a non-blocking open is FILE_FLAG_OVERLAPPED with a pump thread
   (runtime/vyto_win_pump.h) reading into a loopback socket, which is what
   makes it pollable; a blocking open is plain synchronous I/O. */

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#elif !defined(VT_NO_LIBC)
#define _GNU_SOURCE
#endif
#include <stdint.h>

#if defined(VT_NO_LIBC)

int dev_open(const char *path, int mode, int nonblock) { (void)path; (void)mode; (void)nonblock; return -1; }
int64_t dev_read(int fd, char *buf, int64_t cap) { (void)fd; (void)buf; (void)cap; return -2; }
int64_t dev_write(int fd, const char *buf, int64_t n) { (void)fd; (void)buf; (void)n; return -2; }
int64_t dev_seek(int fd, int64_t off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int dev_poll_fd(int fd) { (void)fd; return -1; }
int64_t dev_raw(int fd) { (void)fd; return -1; }
void dev_close(int fd) { (void)fd; }
void *hid_list(void) { return 0; }
int hid_count(void *h) { (void)h; return 0; }
const char *hid_path(void *h, int i) { (void)h; (void)i; return ""; }
const char *hid_name(void *h, int i) { (void)h; (void)i; return ""; }
int hid_vid(void *h, int i) { (void)h; (void)i; return 0; }
int hid_pid(void *h, int i) { (void)h; (void)i; return 0; }
void hid_free(void *h) { (void)h; }

#else /* hosted */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- HID list, shared ------------------------------------------------------ */

typedef struct {
    char path[512];
    char name[128];
    int vid, pid;
} HidDev;

typedef struct {
    HidDev *v;
    int n, cap;
} HidList;

static HidDev *hid_add(HidList *l) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        HidDev *nv = (HidDev *)realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv) return NULL;
        l->v = nv;
        l->cap = nc;
    }
    HidDev *d = &l->v[l->n++];
    memset(d, 0, sizeof *d);
    return d;
}
static HidDev *hid_at(void *h, int i) {
    HidList *l = (HidList *)h;
    return (l && i >= 0 && i < l->n) ? &l->v[i] : NULL;
}
int hid_count(void *h) { return h ? ((HidList *)h)->n : 0; }
const char *hid_path(void *h, int i) { HidDev *d = hid_at(h, i); return d ? d->path : ""; }
const char *hid_name(void *h, int i) { HidDev *d = hid_at(h, i); return d ? d->name : ""; }
int hid_vid(void *h, int i) { HidDev *d = hid_at(h, i); return d ? d->vid : 0; }
int hid_pid(void *h, int i) { HidDev *d = hid_at(h, i); return d ? d->pid : 0; }
void hid_free(void *h) {
    HidList *l = (HidList *)h;
    if (!l) return;
    free(l->v);
    free(l);
}

#ifndef _WIN32
/* ======================================================================
   POSIX
   ====================================================================== */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

/* Open `path`. mode: 0 read, 1 write, 2 read-write, 3 write+create+truncate (for
   writing a new file, e.g. a recording); nonblock adds O_NONBLOCK. Returns the fd, or
   -1 (missing node, no permission). Mode 3 creates the file 0644 if absent. */
int dev_open(const char *path, int mode, int nonblock) {
    int flags = (mode == 1) ? O_WRONLY
              : (mode == 2) ? O_RDWR
              : (mode == 3) ? (O_WRONLY | O_CREAT | O_TRUNC)
              : O_RDONLY;
    if (nonblock) flags |= O_NONBLOCK;
    return open(path, flags | O_CLOEXEC, 0644);
}

/* >0 = bytes read, 0 = EOF, -1 = would-block, -2 = error. */
int64_t dev_read(int fd, char *buf, int64_t cap) {
    ssize_t r = read(fd, buf, (size_t)cap);
    if (r >= 0) return (int64_t)r;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
}

/* >=0 = bytes written, -1 = would-block, -2 = error. */
int64_t dev_write(int fd, const char *buf, int64_t n) {
    ssize_t w = write(fd, buf, (size_t)n);
    if (w >= 0) return (int64_t)w;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
}

/* Reposition. whence: 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END. New offset, or -1. */
int64_t dev_seek(int fd, int64_t off, int whence) {
    off_t r = lseek(fd, (off_t)off, whence);
    return (r == (off_t)-1) ? -1 : (int64_t)r;
}

int dev_poll_fd(int fd) { return fd; }
int64_t dev_raw(int fd) { return fd; }
void dev_close(int fd) { if (fd >= 0) close(fd); }

#ifdef __linux__
/* /sys/class/hidraw/hidrawN/device/uevent has HID_ID=bus:vendor:product (hex)
   and HID_NAME; the node is /dev/hidrawN. */
static int hid_cmp(const void *a, const void *b) {
    return strcmp(((const HidDev *)a)->path, ((const HidDev *)b)->path);
}
void *hid_list(void) {
    HidList *l = (HidList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) return l;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[512], line[512];
        snprintf(p, sizeof p, "/sys/class/hidraw/%.200s/device/uevent", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        HidDev *h = hid_add(l);
        if (!h) { fclose(f); break; }
        snprintf(h->path, sizeof h->path, "/dev/%.200s", e->d_name);
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (strncmp(line, "HID_ID=", 7) == 0) {
                unsigned bus = 0, vid = 0, pid = 0;
                if (sscanf(line + 7, "%x:%x:%x", &bus, &vid, &pid) == 3) {
                    h->vid = (int)(vid & 0xFFFF);
                    h->pid = (int)(pid & 0xFFFF);
                }
            } else if (strncmp(line, "HID_NAME=", 9) == 0) {
                snprintf(h->name, sizeof h->name, "%.120s", line + 9);
            }
        }
        fclose(f);
    }
    closedir(d);
    if (l->n > 1) qsort(l->v, (size_t)l->n, sizeof *l->v, hid_cmp);
    return l;
}
#else
void *hid_list(void) { return calloc(1, sizeof(HidList)); }
#endif

#else
/* ======================================================================
   Windows
   ====================================================================== */
#include "vyto_win_pump.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include "vyto_win.h"

#define DEV_SLOTS 64
#define DEV_WRITE_MS 5000

typedef struct {
    HANDLE h;
    int overlapped;
    VtPump *pump;           /* overlapped handles only */
    OVERLAPPED rov;         /* the pump's read */
    uint64_t roff, woff;    /* file offsets: overlapped I/O has no file pointer */
} DevSlot;

static DevSlot *g_dev[DEV_SLOTS];

static DevSlot *dslot(int fd) { return (fd >= 0 && fd < DEV_SLOTS) ? g_dev[fd] : NULL; }

static int dev_pump_read(void *ctx, char *buf, int cap, HANDLE stop, DWORD *err) {
    DevSlot *s = (DevSlot *)ctx;
    DWORD got = 0;
    ResetEvent(s->rov.hEvent);
    s->rov.Offset = (DWORD)(s->roff & 0xFFFFFFFFu);
    s->rov.OffsetHigh = (DWORD)(s->roff >> 32);
    if (!ReadFile(s->h, buf, (DWORD)cap, NULL, &s->rov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) { *err = e; return VT_PUMP_FAILED; }
        if (e != ERROR_IO_PENDING) { *err = e; return VT_PUMP_FAILED; }
        HANDLE w[2] = { s->rov.hEvent, stop };
        if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(s->h, &s->rov);
            GetOverlappedResult(s->h, &s->rov, &got, TRUE);
            return VT_PUMP_STOPPED;
        }
    }
    if (!GetOverlappedResult(s->h, &s->rov, &got, FALSE)) {
        DWORD e = GetLastError();
        if (e == ERROR_OPERATION_ABORTED && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
            return VT_PUMP_STOPPED;
        *err = e;
        return VT_PUMP_FAILED;
    }
    /* A zero-byte success is end of file (a regular file, a pipe whose
       writer closed): end the stream as POSIX read() returning 0 does. */
    if (got == 0) { *err = ERROR_HANDLE_EOF; return VT_PUMP_FAILED; }
    s->roff += got;
    return (int)got;
}

static void dev_free(int fd) {
    DevSlot *s = dslot(fd);
    if (!s) return;
    if (s->pump) vt_pump_stop(s->pump, s->h);
    if (s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
    if (s->rov.hEvent) CloseHandle(s->rov.hEvent);
    free(s);
    g_dev[fd] = NULL;
}

/* Any path CreateFile takes: a HID interface path from hid_devices(),
   "\\.\COM3", "\\.\pipe\name", a plain file. Shared for reading and writing,
   since a device such as a HID collection is usually open elsewhere too. */
int dev_open(const char *path, int mode, int nonblock) {
    if (!path || !*path) return -1;
    int fd = -1;
    for (int i = 0; i < DEV_SLOTS; i++) if (!g_dev[i]) { fd = i; break; }
    if (fd < 0) return -1;
    DWORD access = mode == 1 || mode == 3 ? GENERIC_WRITE
                 : mode == 2 ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD disp = mode == 3 ? CREATE_ALWAYS : OPEN_EXISTING;
    VtWBuf w;
    vt_wbuf_from(&w, path);
    HANDLE h = CreateFileW(w.p, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp,
                           nonblock ? FILE_FLAG_OVERLAPPED : 0, NULL);
    vt_wbuf_free(&w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DevSlot *s = (DevSlot *)calloc(1, sizeof *s);
    if (!s) { CloseHandle(h); return -1; }
    s->h = h;
    s->overlapped = nonblock != 0;
    g_dev[fd] = s;
    /* Only a readable non-blocking device needs a pump: there is nothing to
       wait for on a write-only one. */
    if (s->overlapped && (access & GENERIC_READ)) {
        s->rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        s->pump = s->rov.hEvent ? vt_pump_start(dev_pump_read, s) : NULL;
        if (!s->pump) { dev_free(fd); return -1; }
    }
    return fd;
}

int64_t dev_read(int fd, char *buf, int64_t cap) {
    DevSlot *s = dslot(fd);
    if (!s) return -2;
    if (cap > 1 << 30) cap = 1 << 30;
    if (s->pump) return vt_pump_recv(s->pump, buf, (int)cap);
    if (s->overlapped) return -2;       /* write-only */
    DWORD got = 0;
    if (!ReadFile(s->h, buf, (DWORD)cap, &got, NULL))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -2;
    return (int64_t)got;
}

int64_t dev_write(int fd, const char *buf, int64_t n) {
    DevSlot *s = dslot(fd);
    if (!s) return -2;
    if (n <= 0) return 0;
    if (n > 1 << 30) n = 1 << 30;
    DWORD put = 0;
    if (!s->overlapped) return WriteFile(s->h, buf, (DWORD)n, &put, NULL) ? (int64_t)put : -2;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)(s->woff & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(s->woff >> 32);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -2;
    int64_t out = -2;
    if (WriteFile(s->h, buf, (DWORD)n, NULL, &ov) || GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, DEV_WRITE_MS) != WAIT_OBJECT_0) CancelIoEx(s->h, &ov);
        if (GetOverlappedResult(s->h, &ov, &put, TRUE) || put > 0) {
            out = put > 0 ? (int64_t)put : -1;
            s->woff += put;
        } else {
            out = GetLastError() == ERROR_OPERATION_ABORTED ? -1 : -2;
        }
    }
    CloseHandle(ov.hEvent);
    return out;
}

/* Blocking handles only: an overlapped one has no file pointer to move. */
int64_t dev_seek(int fd, int64_t off, int whence) {
    DevSlot *s = dslot(fd);
    if (!s || s->overlapped) return -1;
    LARGE_INTEGER d, now;
    d.QuadPart = off;
    DWORD how = whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN;
    if (!SetFilePointerEx(s->h, d, &now, how)) return -1;
    return (int64_t)now.QuadPart;
}

int dev_poll_fd(int fd) {
    DevSlot *s = dslot(fd);
    return (s && s->pump) ? vt_pump_fd(s->pump) : -1;
}
int64_t dev_raw(int fd) {
    DevSlot *s = dslot(fd);
    return s ? (int64_t)(intptr_t)s->h : -1;
}
void dev_close(int fd) { dev_free(fd); }

/* ---- HID listing: the HID device interface class ----------------------- */

static const GUID dev_GUID_HID =
    {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
static const DEVPROPKEY dev_PKEY_BusReportedDeviceDesc =
    {{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};

static void dev_utf8(const WCHAR *w, char *out, size_t cap) {
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    if (k <= 0) out[0] = 0;
    else out[cap - 1] = 0;
}

static int dev_hex_after(const char *s, const char *tag) {
    const char *p = strstr(s, tag);
    if (!p) return 0;
    char b[5];
    memcpy(b, p + strlen(tag), 4);
    b[4] = 0;
    return (int)strtol(b, NULL, 16);
}

/* The name a HID collection shows: the product string of the USB device it
   belongs to (its parent), else its own description. */
static void hid_name_of(HDEVINFO set, SP_DEVINFO_DATA *dd, char *out, size_t cap) {
    WCHAR w[256];
    DEVPROPTYPE pt = 0;
    out[0] = 0;
    DEVINST parent;
    if (CM_Get_Parent(&parent, dd->DevInst, 0) == CR_SUCCESS) {
        ULONG sz = sizeof w - 2;
        if (CM_Get_DevNode_PropertyW(parent, &dev_PKEY_BusReportedDeviceDesc, &pt, (BYTE *)w,
                                     &sz, 0) == CR_SUCCESS &&
            pt == DEVPROP_TYPE_STRING) {
            w[255] = 0;
            dev_utf8(w, out, cap);
            return;
        }
    }
    if (SetupDiGetDeviceRegistryPropertyW(set, dd, SPDRP_DEVICEDESC, NULL, (BYTE *)w,
                                          sizeof w - 2, NULL)) {
        w[255] = 0;
        dev_utf8(w, out, cap);
    }
}

void *hid_list(void) {
    HidList *l = (HidList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    HDEVINFO set = SetupDiGetClassDevsW(&dev_GUID_HID, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return l;
    SP_DEVICE_INTERFACE_DATA id;
    for (DWORD i = 0;; i++) {
        memset(&id, 0, sizeof id);
        id.cbSize = sizeof id;
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &dev_GUID_HID, i, &id)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &id, NULL, 0, &need, NULL);
        if (need == 0) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!det) break;
        det->cbSize = sizeof *det;
        SP_DEVINFO_DATA dd;
        memset(&dd, 0, sizeof dd);
        dd.cbSize = sizeof dd;
        if (SetupDiGetDeviceInterfaceDetailW(set, &id, det, need, NULL, &dd)) {
            HidDev *h = hid_add(l);
            if (h) {
                dev_utf8(det->DevicePath, h->path, sizeof h->path);
                char lower[512];
                size_t k = 0;
                for (; h->path[k] && k < sizeof lower - 1; k++)
                    lower[k] = (char)(h->path[k] >= 'A' && h->path[k] <= 'Z' ? h->path[k] + 32
                                                                              : h->path[k]);
                lower[k] = 0;
                h->vid = dev_hex_after(lower, "vid_");
                h->pid = dev_hex_after(lower, "pid_");
                hid_name_of(set, &dd, h->name, sizeof h->name);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(set);
    return l;
}

#endif /* _WIN32 */
#endif /* hosted */
