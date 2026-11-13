/* vyto/hw/serial native backing — serial ports on POSIX termios and Win32.

   The Vyto-way streaming hardware pattern (docs/HARDWARE.md): an open port
   gives Vyto a pollable fd, so it folds into a PollSet next to sockets,
   windows and workers. Reads/writes use vyto/net/socket's encoded returns
   (>0 bytes, 0 EOF, -1 would-block, -2 error), so a poll-driven loop can tell
   "come back later" from "the port is gone".

   The "fd" handed to Vyto is a handle id: on POSIX the real descriptor; on
   Windows a slot in a small table (a HANDLE does not fit the int the API
   passes around). ser_poll_fd() is what goes into a PollSet: the same
   descriptor on POSIX, the pump's socket on Windows.

   Line settings: data bits 5..8, parity (SER_PARITY_*), stop bits 1 or 2,
   flow control (SER_FLOW_*), and DTR/RTS asserted or not at open. */

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   /* Vista: CancelIoEx, for the pump */
#endif
#elif !defined(VT_NO_LIBC)
#define _GNU_SOURCE
#endif
#include <stdint.h>

#define SER_PARITY_NONE  0
#define SER_PARITY_ODD   1
#define SER_PARITY_EVEN  2
#define SER_PARITY_MARK  3
#define SER_PARITY_SPACE 4

#define SER_FLOW_NONE    0
#define SER_FLOW_RTSCTS  1
#define SER_FLOW_XONXOFF 2

#define SER_MODEM_CTS 1
#define SER_MODEM_DSR 2
#define SER_MODEM_RI  4
#define SER_MODEM_DCD 8

#if defined(VT_NO_LIBC)
/* --freestanding: no OS, so no ports. Every entry point reports failure. */

int ser_open_ex(const char *p, int b, int d, int pa, int s, int f, int dtr, int rts) {
    (void)p; (void)b; (void)d; (void)pa; (void)s; (void)f; (void)dtr; (void)rts;
    return -1;
}
int ser_open(const char *p, int b) { (void)p; (void)b; return -1; }
int ser_configure(int fd, int b) { (void)fd; (void)b; return -1; }
int ser_set_line(int fd, int b, int d, int pa, int s, int f) {
    (void)fd; (void)b; (void)d; (void)pa; (void)s; (void)f;
    return -1;
}
int ser_poll_fd(int fd) { (void)fd; return -1; }
int64_t ser_read(int fd, char *buf, int64_t cap) { (void)fd; (void)buf; (void)cap; return -2; }
int64_t ser_write(int fd, const char *buf, int64_t n) { (void)fd; (void)buf; (void)n; return -2; }
int ser_drain(int fd) { (void)fd; return -1; }
int ser_flush(int fd) { (void)fd; return -1; }
int ser_set_dtr(int fd, int on) { (void)fd; (void)on; return -1; }
int ser_set_rts(int fd, int on) { (void)fd; (void)on; return -1; }
int ser_modem(int fd) { (void)fd; return -1; }
int ser_break(int fd, int ms) { (void)fd; (void)ms; return -1; }
const char *ser_error(int fd) { (void)fd; return "no serial ports in this build"; }
void ser_close(int fd) { (void)fd; }
void *ser_ports(void) { return 0; }
int ser_port_count(void *h) { (void)h; return 0; }
const char *ser_port_path(void *h, int i) { (void)h; (void)i; return ""; }
const char *ser_port_name(void *h, int i) { (void)h; (void)i; return ""; }
const char *ser_port_mfr(void *h, int i) { (void)h; (void)i; return ""; }
const char *ser_port_serial(void *h, int i) { (void)h; (void)i; return ""; }
const char *ser_port_kind(void *h, int i) { (void)h; (void)i; return ""; }
int ser_port_vid(void *h, int i) { (void)h; (void)i; return 0; }
int ser_port_pid(void *h, int i) { (void)h; (void)i; return 0; }
void ser_ports_free(void *h) { (void)h; }

#else /* hosted */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- the port list, shared by both arms ---------------------------------- */

typedef struct {
    char path[64];       /* "/dev/ttyUSB0", "COM3" */
    char name[128];      /* what the system calls it: "USB Serial Port (COM3)" */
    char mfr[96];
    char serial[96];
    char kind[16];       /* "usb", "native", "bluetooth", "virtual" */
    int vid, pid;
} SerPort;

typedef struct {
    SerPort *v;
    int n, cap;
} SerPorts;

static SerPort *ports_add(SerPorts *l) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        SerPort *nv = (SerPort *)realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv) return NULL;
        l->v = nv;
        l->cap = nc;
    }
    SerPort *p = &l->v[l->n++];
    memset(p, 0, sizeof *p);
    return p;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static SerPort *port_at(void *h, int i) {
    SerPorts *l = (SerPorts *)h;
    if (!l || i < 0 || i >= l->n) return NULL;
    return &l->v[i];
}
int ser_port_count(void *h) { return h ? ((SerPorts *)h)->n : 0; }
const char *ser_port_path(void *h, int i)   { SerPort *p = port_at(h, i); return p ? p->path : ""; }
const char *ser_port_name(void *h, int i)   { SerPort *p = port_at(h, i); return p ? p->name : ""; }
const char *ser_port_mfr(void *h, int i)    { SerPort *p = port_at(h, i); return p ? p->mfr : ""; }
const char *ser_port_serial(void *h, int i) { SerPort *p = port_at(h, i); return p ? p->serial : ""; }
const char *ser_port_kind(void *h, int i)   { SerPort *p = port_at(h, i); return p ? p->kind : ""; }
int ser_port_vid(void *h, int i) { SerPort *p = port_at(h, i); return p ? p->vid : 0; }
int ser_port_pid(void *h, int i) { SerPort *p = port_at(h, i); return p ? p->pid : 0; }
void ser_ports_free(void *h) {
    SerPorts *l = (SerPorts *)h;
    if (!l) return;
    free(l->v);
    free(l);
}

static int line_ok(int databits, int parity, int stopbits, int flow) {
    return databits >= 5 && databits <= 8 && parity >= SER_PARITY_NONE &&
           parity <= SER_PARITY_SPACE && (stopbits == 1 || stopbits == 2) &&
           flow >= SER_FLOW_NONE && flow <= SER_FLOW_XONXOFF;
}

#ifndef _WIN32
/* ======================================================================
   POSIX: termios on a non-blocking descriptor. Linux, macOS, BSD.
   ====================================================================== */

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <time.h>

/* The errno of the last failure on each descriptor, for ser_error. */
#define SER_ERRS 1024
static int g_errs[SER_ERRS];
static int fail(int fd) {
    if (fd >= 0 && fd < SER_ERRS) g_errs[fd] = errno;
    return -1;
}

/* An integer baud to its termios speed constant, or B0 if this system has
   none. Arbitrary rates (250000) need Linux termios2 and are not offered. */
static speed_t baud_const(int baud) {
    switch (baud) {
        case 50:      return B50;
        case 75:      return B75;
        case 110:     return B110;
        case 300:     return B300;
        case 600:     return B600;
        case 1200:    return B1200;
        case 2400:    return B2400;
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
#ifdef B460800
        case 460800:  return B460800;
#endif
#ifdef B500000
        case 500000:  return B500000;
#endif
#ifdef B576000
        case 576000:  return B576000;
#endif
#ifdef B921600
        case 921600:  return B921600;
#endif
#ifdef B1000000
        case 1000000: return B1000000;
#endif
#ifdef B1500000
        case 1500000: return B1500000;
#endif
#ifdef B2000000
        case 2000000: return B2000000;
#endif
#ifdef B3000000
        case 3000000: return B3000000;
#endif
        default:      return B0;
    }
}

/* The whole line: raw, receiver on, modem-status lines ignored for open/read
   (CLOCAL), then the caller's framing and flow control. 0 ok, -1 error. */
int ser_set_line(int fd, int baud, int databits, int parity, int stopbits, int flow) {
    speed_t sp = baud_const(baud);
    if (sp == B0 || !line_ok(databits, parity, stopbits, flow)) { errno = EINVAL; return fail(fd); }
#ifndef CMSPAR
    if (parity == SER_PARITY_MARK || parity == SER_PARITY_SPACE) { errno = EINVAL; return fail(fd); }
#endif
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return fail(fd);
    cfmakeraw(&t);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~(tcflag_t)CSIZE;
    t.c_cflag |= databits == 5 ? CS5 : databits == 6 ? CS6 : databits == 7 ? CS7 : CS8;
    if (stopbits == 2) t.c_cflag |= CSTOPB; else t.c_cflag &= ~(tcflag_t)CSTOPB;
    t.c_cflag &= ~(tcflag_t)(PARENB | PARODD);
#ifdef CMSPAR
    t.c_cflag &= ~(tcflag_t)CMSPAR;
#endif
    if (parity != SER_PARITY_NONE) {
        t.c_cflag |= PARENB;
        if (parity == SER_PARITY_ODD) t.c_cflag |= PARODD;
#ifdef CMSPAR
        /* mark = stick parity 1 (CMSPAR|PARODD), space = stick 0 (CMSPAR) */
        if (parity == SER_PARITY_MARK) t.c_cflag |= CMSPAR | PARODD;
        if (parity == SER_PARITY_SPACE) t.c_cflag |= CMSPAR;
#endif
    }
#ifdef CRTSCTS
    if (flow == SER_FLOW_RTSCTS) t.c_cflag |= CRTSCTS; else t.c_cflag &= ~(tcflag_t)CRTSCTS;
#else
    if (flow == SER_FLOW_RTSCTS) { errno = EINVAL; return fail(fd); }
#endif
    if (flow == SER_FLOW_XONXOFF) t.c_iflag |= IXON | IXOFF;
    else t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    /* VMIN 1, not 0: with VMIN=0 VTIME=0 a Linux tty read returns 0 when
       there is no data, even on a non-blocking descriptor, which is
       indistinguishable from a hangup. With VMIN=1 the O_NONBLOCK flag
       decides and "no data" is EAGAIN, so 0 can only mean the line hung up. */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t) != 0) return fail(fd);
    return 0;
}

/* Change only the speed, keeping framing and flow control. */
int ser_configure(int fd, int baud) {
    speed_t sp = baud_const(baud);
    if (sp == B0) { errno = EINVAL; return fail(fd); }
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return fail(fd);
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t) != 0) return fail(fd);
    return 0;
}

static int modem_bit(int fd, int bit, int on) {
    return ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &bit) == 0 ? 0 : fail(fd);
}
int ser_set_dtr(int fd, int on) { return modem_bit(fd, TIOCM_DTR, on); }
int ser_set_rts(int fd, int on) { return modem_bit(fd, TIOCM_RTS, on); }

int ser_modem(int fd) {
    int st = 0;
    if (ioctl(fd, TIOCMGET, &st) != 0) return fail(fd);
    int out = 0;
    if (st & TIOCM_CTS) out |= SER_MODEM_CTS;
    if (st & TIOCM_DSR) out |= SER_MODEM_DSR;
    if (st & TIOCM_RNG) out |= SER_MODEM_RI;
    if (st & TIOCM_CAR) out |= SER_MODEM_DCD;
    return out;
}

/* Hold the line in break for `ms`, then release it. */
int ser_break(int fd, int ms) {
    if (ms <= 0) ms = 250;
    if (ioctl(fd, TIOCSBRK, 0) != 0) return fail(fd);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
    if (ioctl(fd, TIOCCBRK, 0) != 0) return fail(fd);
    return 0;
}

/* Open non-blocking and configure. The descriptor, or -1 (missing device,
   bad settings, no permission). DTR and RTS are asserted unless asked not
   to: many USB CDC boards stay silent until DTR is up. */
int ser_open_ex(const char *path, int baud, int databits, int parity, int stopbits, int flow,
                int dtr, int rts) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ser_set_line(fd, baud, databits, parity, stopbits, flow) != 0) { close(fd); return -1; }
    /* Some ports (ptys) have no modem lines: not being able to set them is
       not a reason to refuse the port. */
    ser_set_dtr(fd, dtr);
    if (flow != SER_FLOW_RTSCTS) ser_set_rts(fd, rts);
    if (fd < SER_ERRS) g_errs[fd] = 0;
    return fd;
}
int ser_open(const char *path, int baud) {
    return ser_open_ex(path, baud, 8, SER_PARITY_NONE, 1, SER_FLOW_NONE, 1, 1);
}

int ser_poll_fd(int fd) { return fd; }

/* >0 bytes, 0 EOF (the port hung up), -1 nothing yet, -2 error. */
int64_t ser_read(int fd, char *buf, int64_t cap) {
    ssize_t r = read(fd, buf, (size_t)cap);
    if (r > 0) return (int64_t)r;
    if (r == 0) { errno = EIO; fail(fd); return 0; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
    fail(fd);
    return -2;
}

/* >=0 bytes written, -1 would-block, -2 error. */
int64_t ser_write(int fd, const char *buf, int64_t n) {
    ssize_t w = write(fd, buf, (size_t)n);
    if (w >= 0) return (int64_t)w;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -1;
    fail(fd);
    return -2;
}

int ser_drain(int fd) { return tcdrain(fd) == 0 ? 0 : fail(fd); }
int ser_flush(int fd) { return tcflush(fd, TCIOFLUSH) == 0 ? 0 : fail(fd); }

const char *ser_error(int fd) {
    int e = (fd >= 0 && fd < SER_ERRS) ? g_errs[fd] : 0;
    return e ? strerror(e) : "";
}

void ser_close(int fd) {
    if (fd < 0) return;
    if (fd < SER_ERRS) g_errs[fd] = 0;
    close(fd);
}

/* ---- listing: /sys/class/tty (Linux) ------------------------------------- */

#ifdef __linux__
static int read_line(const char *path, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
    return out[0] != 0;
}

/* basename of where `link` points, or "" */
static void link_base(const char *link, char *out, size_t cap) {
    char tgt[PATH_MAX];
    ssize_t n = readlink(link, tgt, sizeof tgt - 1);
    out[0] = 0;
    if (n <= 0) return;
    tgt[n] = 0;
    const char *b = strrchr(tgt, '/');
    copy_str(out, cap, b ? b + 1 : tgt);
}

static int cmp_ports(const void *a, const void *b) {
    return strcmp(((const SerPort *)a)->path, ((const SerPort *)b)->path);
}

void *ser_ports(void) {
    SerPorts *l = (SerPorts *)calloc(1, sizeof *l);
    if (!l) return NULL;
    DIR *d = opendir("/sys/class/tty");
    if (!d) return l;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char dev[PATH_MAX], drv[64], sub[64], lk[PATH_MAX + 16];
        /* No device link: a virtual terminal, a pty, the console. */
        snprintf(dev, sizeof dev, "/sys/class/tty/%s/device", e->d_name);
        char real[PATH_MAX];
        if (!realpath(dev, real)) continue;
        snprintf(lk, sizeof lk, "%s/driver", dev);
        link_base(lk, drv, sizeof drv);
        if (!drv[0]) continue;
        /* The 8250 driver registers ttyS0..31 whether or not a UART is behind
           them. The serial core says which are empty: its `type` attribute
           is 0 (PORT_UNKNOWN) for a port with no hardware. Kernels before
           6.5 hang those placeholders off the platform bus, and some lack
           `type`, so that check stays as the fallback; a real built-in
           port is on pnp or pci. */
        char ty[16];
        snprintf(lk, sizeof lk, "/sys/class/tty/%.200s/type", e->d_name);
        if (read_line(lk, ty, sizeof ty) && strcmp(ty, "0") == 0) continue;
        snprintf(lk, sizeof lk, "%s/subsystem", dev);
        link_base(lk, sub, sizeof sub);
        if (strncmp(e->d_name, "ttyS", 4) == 0 && strcmp(sub, "platform") == 0) continue;

        SerPort *p = ports_add(l);
        if (!p) break;
        snprintf(p->path, sizeof p->path, "/dev/%.58s", e->d_name);
        copy_str(p->kind, sizeof p->kind,
                 strncmp(e->d_name, "rfcomm", 6) == 0 ? "bluetooth" : "native");
        /* A USB serial port's device is an interface; the USB device holding
           idVendor/idProduct/serial is one or two directories up. */
        char up[PATH_MAX];
        copy_str(up, sizeof up, real);
        for (int depth = 0; depth < 4; depth++) {
            char f[PATH_MAX + 32], v[96];
            snprintf(f, sizeof f, "%s/idVendor", up);
            if (read_line(f, v, sizeof v)) {
                p->vid = (int)strtol(v, NULL, 16);
                snprintf(f, sizeof f, "%s/idProduct", up);
                if (read_line(f, v, sizeof v)) p->pid = (int)strtol(v, NULL, 16);
                snprintf(f, sizeof f, "%s/manufacturer", up);
                read_line(f, p->mfr, sizeof p->mfr);
                snprintf(f, sizeof f, "%s/serial", up);
                read_line(f, p->serial, sizeof p->serial);
                snprintf(f, sizeof f, "%s/product", up);
                read_line(f, p->name, sizeof p->name);
                copy_str(p->kind, sizeof p->kind, "usb");
                break;
            }
            char *slash = strrchr(up, '/');
            if (!slash || slash == up) break;
            *slash = 0;
        }
        if (!p->name[0]) copy_str(p->name, sizeof p->name, e->d_name);
    }
    closedir(d);
    if (l->n > 1) qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_ports);
    return l;
}
#else
/* macOS / BSD: no listing yet. An empty list, not a failure. */
void *ser_ports(void) { return calloc(1, sizeof(SerPorts)); }
#endif

#else
/* ======================================================================
   Windows: COM ports, made pollable by a pump thread (vyto_win_pump.h).

   The port is opened FILE_FLAG_OVERLAPPED. That is not optional once a
   second thread reads it: on a handle opened without it Windows serializes
   all I/O, so a blocking ReadFile on the pump would stall every WriteFile
   from the Vyto thread until a byte arrived.

   Reads: the pump thread keeps one overlapped ReadFile in flight, which
   completes once bytes have arrived and the line has gone quiet (see the
   COMMTIMEOUTS in ser_open_ex). What it reads goes into the pump's socket.
   Writes: from the Vyto thread, overlapped, waited for a bounded time and
   cancelled past it, so a stalled line (flow control held off) cannot hang
   the program.
   Unplug: the in-flight read fails, the pump records why and closes its
   socket end; Vyto reads EOF and ser_error() gives the reason.
   ====================================================================== */

#include "vyto_win_pump.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include "vyto_win.h"

#define SER_SLOTS 64
#define SER_WRITE_MS 1000   /* the longest a write may wait on the line */

typedef struct {
    HANDLE h;
    VtPump *pump;
    OVERLAPPED rov;          /* the pump thread's read */
    DWORD err;               /* the last failure outside the pump */
    char errtext[256];
} SerSlot;

static SerSlot *g_slots[SER_SLOTS];

static SerSlot *slot(int fd) {
    return (fd >= 0 && fd < SER_SLOTS) ? g_slots[fd] : NULL;
}
static int fail_w(SerSlot *s) {
    if (s) s->err = GetLastError();
    return -1;
}

/* The pump's read: one overlapped ReadFile, waited for together with the
   stop event. */
static int ser_pump_read(void *ctx, char *buf, int cap, HANDLE stop, DWORD *err) {
    SerSlot *s = (SerSlot *)ctx;
    DWORD got = 0;
    ResetEvent(s->rov.hEvent);
    if (!ReadFile(s->h, buf, (DWORD)cap, NULL, &s->rov)) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) { *err = e; return VT_PUMP_FAILED; }
        HANDLE w[2] = { s->rov.hEvent, stop };
        DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0) {
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
    return (int)got;
}

int ser_set_line(int fd, int baud, int databits, int parity, int stopbits, int flow) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    if (baud <= 0 || !line_ok(databits, parity, stopbits, flow)) {
        s->err = ERROR_INVALID_PARAMETER;
        return -1;
    }
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(s->h, &dcb)) return fail_w(s);
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = (BYTE)databits;
    dcb.Parity = parity == SER_PARITY_ODD ? ODDPARITY : parity == SER_PARITY_EVEN ? EVENPARITY
               : parity == SER_PARITY_MARK ? MARKPARITY : parity == SER_PARITY_SPACE ? SPACEPARITY
               : NOPARITY;
    dcb.fParity = parity != SER_PARITY_NONE;
    dcb.StopBits = stopbits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = flow == SER_FLOW_RTSCTS;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = dcb.fInX = flow == SER_FLOW_XONXOFF;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    dcb.fNull = FALSE;
    /* A framing or parity error must not stall every later read until
       someone calls ClearCommError. */
    dcb.fAbortOnError = FALSE;
    if (flow == SER_FLOW_RTSCTS) dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
    else if (dcb.fRtsControl == RTS_CONTROL_HANDSHAKE) dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(s->h, &dcb)) return fail_w(s);
    return 0;
}

int ser_configure(int fd, int baud) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(s->h, &dcb)) return fail_w(s);
    dcb.BaudRate = (DWORD)baud;
    if (baud <= 0 || !SetCommState(s->h, &dcb)) return fail_w(s);
    return 0;
}

static int escape(int fd, DWORD fn) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    return EscapeCommFunction(s->h, fn) ? 0 : fail_w(s);
}
int ser_set_dtr(int fd, int on) { return escape(fd, on ? SETDTR : CLRDTR); }
int ser_set_rts(int fd, int on) { return escape(fd, on ? SETRTS : CLRRTS); }

int ser_modem(int fd) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    DWORD st = 0;
    if (!GetCommModemStatus(s->h, &st)) return fail_w(s);
    int out = 0;
    if (st & MS_CTS_ON) out |= SER_MODEM_CTS;
    if (st & MS_DSR_ON) out |= SER_MODEM_DSR;
    if (st & MS_RING_ON) out |= SER_MODEM_RI;
    if (st & MS_RLSD_ON) out |= SER_MODEM_DCD;
    return out;
}

int ser_break(int fd, int ms) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    if (ms <= 0) ms = 250;
    if (!SetCommBreak(s->h)) return fail_w(s);
    Sleep((DWORD)ms);
    if (!ClearCommBreak(s->h)) return fail_w(s);
    return 0;
}

static void slot_free(int fd) {
    SerSlot *s = slot(fd);
    if (!s) return;
    if (s->pump) vt_pump_stop(s->pump, s->h);
    if (s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
    if (s->rov.hEvent) CloseHandle(s->rov.hEvent);
    free(s);
    g_slots[fd] = NULL;
}

/* "COM3" or "\\.\COM3" (the prefix is what makes COM10 and up openable),
   or any other device path Windows accepts, such as "\\.\CNCA0". */
int ser_open_ex(const char *path, int baud, int databits, int parity, int stopbits, int flow,
                int dtr, int rts) {
    if (!path || !*path) return -1;
    int fd = -1;
    for (int i = 0; i < SER_SLOTS; i++) if (!g_slots[i]) { fd = i; break; }
    if (fd < 0) return -1;
    char name[300];
    if (strncmp(path, "\\\\", 2) == 0) snprintf(name, sizeof name, "%s", path);
    else snprintf(name, sizeof name, "\\\\.\\%s", path);
    VtWBuf w;
    vt_wbuf_from(&w, name);
    HANDLE h = CreateFileW(w.p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    vt_wbuf_free(&w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    SerSlot *s = (SerSlot *)calloc(1, sizeof *s);
    if (!s) { CloseHandle(h); return -1; }
    s->h = h;
    g_slots[fd] = s;
    s->rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s->rov.hEvent || ser_set_line(fd, baud, databits, parity, stopbits, flow) != 0) {
        slot_free(fd);
        return -1;
    }
    /* DTR/RTS as asked: asserted by default, which CDC boards need before
       they send anything (and which resets an Arduino Uno, as its IDE does). */
    ser_set_dtr(fd, dtr);
    if (flow != SER_FLOW_RTSCTS) ser_set_rts(fd, rts);
    /* An interval timeout and no total timeout: a read waits for the first
       byte however long that takes, then returns once the line has been
       quiet for 1 ms (or the buffer is full). So the pump sleeps in the
       kernel while the port is idle and hands over a burst as one chunk.
       Stopping does not need a timeout: the pump waits on its stop event
       too and cancels the read.

       Not the MAXDWORD/MAXDWORD/constant form ("return when one byte is
       in, else after N ms"), which means the same on Windows but never
       completes under Wine 6; and not WaitCommEvent(EV_RXCHAR), which Wine 6
       completes at once with an empty mask. This form behaves the same on
       both, so the automated Wine run tests the code Windows runs. */
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = 1;
    to.WriteTotalTimeoutConstant = SER_WRITE_MS;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s->pump = vt_pump_start(ser_pump_read, s);
    if (!s->pump) { slot_free(fd); return -1; }
    return fd;
}
int ser_open(const char *path, int baud) {
    return ser_open_ex(path, baud, 8, SER_PARITY_NONE, 1, SER_FLOW_NONE, 1, 1);
}

int ser_poll_fd(int fd) {
    SerSlot *s = slot(fd);
    return s ? vt_pump_fd(s->pump) : -1;
}

int64_t ser_read(int fd, char *buf, int64_t cap) {
    SerSlot *s = slot(fd);
    if (!s) return -2;
    if (cap > 1 << 30) cap = 1 << 30;
    int n = vt_pump_recv(s->pump, buf, (int)cap);
    if (n == -2) s->err = (DWORD)WSAGetLastError();
    return (int64_t)n;
}

int64_t ser_write(int fd, const char *buf, int64_t n) {
    SerSlot *s = slot(fd);
    if (!s) return -2;
    if (n <= 0) return 0;
    if (n > 1 << 30) n = 1 << 30;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) { fail_w(s); return -2; }
    DWORD put = 0;
    int64_t out;
    if (WriteFile(s->h, buf, (DWORD)n, NULL, &ov) ||
        GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, SER_WRITE_MS + 100) != WAIT_OBJECT_0)
            CancelIoEx(s->h, &ov);
        if (GetOverlappedResult(s->h, &ov, &put, TRUE) || put > 0) {
            out = put > 0 ? (int64_t)put : -1;
        } else {
            DWORD e = GetLastError();
            /* timed out with nothing sent: the line is held off */
            out = (e == ERROR_OPERATION_ABORTED || e == ERROR_SEM_TIMEOUT) ? -1 : -2;
            if (out == -2) s->err = e;
        }
    } else {
        s->err = GetLastError();
        out = -2;
    }
    CloseHandle(ov.hEvent);
    return out;
}

int ser_drain(int fd) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    return FlushFileBuffers(s->h) ? 0 : fail_w(s);
}
int ser_flush(int fd) {
    SerSlot *s = slot(fd);
    if (!s) return -1;
    return PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR) ? 0 : fail_w(s);
}

const char *ser_error(int fd) {
    SerSlot *s = slot(fd);
    if (!s) return "";
    DWORD e = vt_pump_error(s->pump);
    if (!e) e = s->err;
    if (!e) return "";
    vt_pump_errtext(e, s->errtext, (int)sizeof s->errtext);
    if (!s->errtext[0]) snprintf(s->errtext, sizeof s->errtext, "Windows error %lu", (unsigned long)e);
    return s->errtext;
}

void ser_close(int fd) { slot_free(fd); }

/* ---- listing: SetupAPI, plus the SERIALCOMM registry map ----------------- */

static const GUID ser_GUID_DEVCLASS_PORTS =
    {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

static void to_utf8(const WCHAR *w, char *out, size_t cap) {
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    if (k <= 0) out[0] = 0;
    else out[cap - 1] = 0;
}

static void reg_prop(HDEVINFO set, SP_DEVINFO_DATA *dd, DWORD prop, char *out, size_t cap) {
    WCHAR w[512];
    DWORD type = 0;
    out[0] = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, dd, prop, &type, (BYTE *)w, sizeof w - 2, NULL)) {
        w[511] = 0;
        to_utf8(w, out, cap);
    }
}

static int hex4_after(const char *s, const char *tag) {
    const char *p = strstr(s, tag);
    if (!p) return 0;
    return (int)strtol(p + strlen(tag), NULL, 16);
}

/* The serial number in a device instance ID, if the device has one:
   "USB\VID_2341&PID_0043\8573632383835" -> its last part (Windows makes up
   IDs containing '&' for devices without one); FTDI's own bus driver puts it
   after the second '+': "FTDIBUS\VID_0403+PID_6001+A50285BIA\0000". */
static int serial_of(const char *id, char *out, size_t cap) {
    out[0] = 0;
    if (strncmp(id, "FTDIBUS\\", 8) == 0) {
        const char *a = strchr(id, '+');
        const char *b = a ? strchr(a + 1, '+') : NULL;
        const char *e = b ? strchr(b + 1, '\\') : NULL;
        if (b && e && e > b + 1) {
            size_t n = (size_t)(e - b - 1);
            if (n >= cap) n = cap - 1;
            memcpy(out, b + 1, n);
            out[n] = 0;
            return 1;
        }
        return 0;
    }
    const char *last = strrchr(id, '\\');
    if (!last || !last[1] || strchr(last, '&')) return 0;
    copy_str(out, cap, last + 1);
    return 1;
}

static int have_path(SerPorts *l, const char *path) {
    for (int i = 0; i < l->n; i++) if (_stricmp(l->v[i].path, path) == 0) return 1;
    return 0;
}

static void list_class_ports(SerPorts *l) {
    HDEVINFO set = SetupDiGetClassDevsW(&ser_GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dd;
    for (DWORD i = 0;; i++) {
        memset(&dd, 0, sizeof dd);
        dd.cbSize = sizeof dd;
        if (!SetupDiEnumDeviceInfo(set, i, &dd)) break;
        HKEY k = SetupDiOpenDevRegKey(set, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        WCHAR wp[64];
        DWORD type = 0, sz = sizeof wp - 2;
        LONG r = RegQueryValueExW(k, L"PortName", NULL, &type, (BYTE *)wp, &sz);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        wp[63] = 0;
        char path[64];
        to_utf8(wp, path, sizeof path);
        if (!path[0] || _strnicmp(path, "LPT", 3) == 0 || have_path(l, path)) continue;

        SerPort *p = ports_add(l);
        if (!p) break;
        copy_str(p->path, sizeof p->path, path);
        reg_prop(set, &dd, SPDRP_FRIENDLYNAME, p->name, sizeof p->name);
        reg_prop(set, &dd, SPDRP_MFG, p->mfr, sizeof p->mfr);
        char hwid[256];
        reg_prop(set, &dd, SPDRP_HARDWAREID, hwid, sizeof hwid);
        p->vid = hex4_after(hwid, "VID_");
        p->pid = hex4_after(hwid, "PID_");
        const char *kind = "native";
        if (_strnicmp(hwid, "USB\\", 4) == 0 || _strnicmp(hwid, "FTDIBUS\\", 8) == 0 ||
            p->vid)
            kind = "usb";
        else if (_strnicmp(hwid, "BTHENUM\\", 8) == 0 || strstr(hwid, "BTHENUM"))
            kind = "bluetooth";
        else if (_strnicmp(hwid, "ROOT\\", 5) == 0 || _strnicmp(hwid, "COM0COM", 7) == 0)
            kind = "virtual";
        copy_str(p->kind, sizeof p->kind, kind);

        /* The serial number lives in the instance ID: the port's own, or for
           a composite device (one of several interfaces) its parent's. Only
           a USB device's ID carries one; elsewhere the last part is just the
           device's own name (com0com's "COM0COM\PORT\CNCA1"). */
        WCHAR wid[512];
        char id[512];
        if (strcmp(kind, "usb") == 0 && SetupDiGetDeviceInstanceIdW(set, &dd, wid, 511, NULL)) {
            to_utf8(wid, id, sizeof id);
            if (!serial_of(id, p->serial, sizeof p->serial)) {
                DEVINST parent;
                if (CM_Get_Parent(&parent, dd.DevInst, 0) == CR_SUCCESS &&
                    CM_Get_Device_IDW(parent, wid, 511, 0) == CR_SUCCESS) {
                    to_utf8(wid, id, sizeof id);
                    if (_strnicmp(id, "USB\\", 4) == 0) serial_of(id, p->serial, sizeof p->serial);
                }
            }
        }
        if (!p->name[0]) copy_str(p->name, sizeof p->name, path);
    }
    SetupDiDestroyDeviceInfoList(set);
}

/* HKLM\HARDWARE\DEVICEMAP\SERIALCOMM names every port a serial driver has
   open for business, including those whose driver is not in the Ports class
   (com0com's pairs, some Bluetooth stacks). Anything listed there and not
   found above is added from it alone. */
static void list_serialcomm(SerPorts *l) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) !=
        ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; i++) {
        WCHAR vname[256], data[64];
        DWORD nlen = 255, dlen = sizeof data - 2, type = 0;
        LONG r = RegEnumValueW(k, i, vname, &nlen, NULL, &type, (BYTE *)data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        data[dlen / sizeof(WCHAR) < 63 ? dlen / sizeof(WCHAR) : 63] = 0;
        vname[255] = 0;
        char path[64], dev[256];
        to_utf8(data, path, sizeof path);
        to_utf8(vname, dev, sizeof dev);
        if (!path[0] || have_path(l, path)) continue;
        SerPort *p = ports_add(l);
        if (!p) break;
        copy_str(p->path, sizeof p->path, path);
        copy_str(p->name, sizeof p->name, path);
        const char *kind = "native";
        if (strstr(dev, "com0com") || strstr(dev, "VCP") || strstr(dev, "Virtual")) kind = "virtual";
        else if (strstr(dev, "Bth") || strstr(dev, "BT")) kind = "bluetooth";
        else if (strstr(dev, "USB") || strstr(dev, "usb")) kind = "usb";
        copy_str(p->kind, sizeof p->kind, kind);
    }
    RegCloseKey(k);
}

/* COM2 before COM10: by the number, then by name. */
static int port_num(const char *p) {
    return _strnicmp(p, "COM", 3) == 0 ? atoi(p + 3) : 100000;
}
static int cmp_ports(const void *a, const void *b) {
    const char *x = ((const SerPort *)a)->path, *y = ((const SerPort *)b)->path;
    int nx = port_num(x), ny = port_num(y);
    if (nx != ny) return nx < ny ? -1 : 1;
    return _stricmp(x, y);
}

void *ser_ports(void) {
    SerPorts *l = (SerPorts *)calloc(1, sizeof *l);
    if (!l) return NULL;
    list_class_ports(l);
    list_serialcomm(l);
    if (l->n > 1) qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_ports);
    return l;
}

#endif /* _WIN32 */
#endif /* hosted */
