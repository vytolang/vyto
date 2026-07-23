/* vyto/hw/serial native backing — serial / TTY ports over POSIX termios.

   The Vyto-way streaming hardware pattern (docs/HARDWARE.md): the port is a fd,
   opened non-blocking so it folds straight into a PollSet event loop next to
   sockets, windows, and workers. Reads/writes are the same encoded-return shape
   as vyto/net/socket's try_* calls (>0 bytes, 0 EOF, -1 would-block, -2 error)
   so a poll-driven loop can tell "come back later" from a real failure. Raw mode
   (no line discipline, no echo, byte-for-byte) at a configurable baud, 8N1.

   POSIX (Linux + macOS + BSD). Windows serial is a separate COM-port backend. */

#define _GNU_SOURCE
#include <stdlib.h>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Map an integer baud to its termios speed_t constant, or B0 if unsupported. */
static speed_t baud_const(int baud) {
    switch (baud) {
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
#ifdef B921600
        case 921600:  return B921600;
#endif
        default:      return B0;
    }
}

/* Configure an open fd as a raw 8N1 port at `baud`. 0 ok, -1 error. */
int ser_configure(int fd, int baud) {
    speed_t sp = baud_const(baud);
    if (sp == B0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return -1;
    cfmakeraw(&t);                 /* no echo, no signals, byte-for-byte */
    t.c_cflag |= (CLOCAL | CREAD); /* ignore modem lines, enable receiver */
    t.c_cflag &= ~CSTOPB;          /* 1 stop bit */
    t.c_cflag &= ~PARENB;          /* no parity */
    t.c_cflag &= ~CSIZE;
    t.c_cflag |= CS8;              /* 8 data bits */
    t.c_cc[VMIN] = 0;              /* non-blocking: never wait for a min count */
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t) != 0) return -1;
    return 0;
}

/* Open `path` (e.g. "/dev/ttyUSB0") non-blocking and configure it. Returns the
   fd, or -1 on failure (missing device, bad baud, no permission). */
int ser_open(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (ser_configure(fd, baud) != 0) { close(fd); return -1; }
    return fd;
}

/* >0 = bytes read, 0 = EOF, -1 = would-block (no data yet), -2 = error. */
long ser_read(int fd, char *buf, long cap) {
    long r = (long)read(fd, buf, (size_t)cap);
    if (r >= 0) return r;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* >=0 = bytes written, -1 = would-block, -2 = error. */
long ser_write(int fd, const char *buf, long n) {
    long w = (long)write(fd, buf, (size_t)n);
    if (w >= 0) return w;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* Block until all queued output bytes have been transmitted. */
int ser_drain(int fd) { return tcdrain(fd); }
/* Discard unread input and unsent output. */
int ser_flush(int fd) { return tcflush(fd, TCIOFLUSH); }

void ser_close(int fd) { if (fd >= 0) close(fd); }

#else /* _WIN32: COM-port backend.

   UNTESTED — written from the Win32 API spec, never run (this repo has no Windows or
   mingw toolchain). Compiles as reference; validate on real hardware before trusting.

   Two Windows realities shape it:
     - A serial port is a HANDLE, not a small int, and a HANDLE does not fit in the
       int fd the Vyto API passes around (it would truncate on 64-bit). So HANDLEs live
       in a small table and the "fd" is a table slot — the same trick the byte-level
       API needs anyway.
     - Windows readiness is IOCP/overlapped, not epoll, so this backend gives byte-
       level read/write with immediate-return timeouts (the encoded -1 would-block still
       works) but does NOT fold into the epoll-based PollSet. Loop integration on
       Windows waits on a Windows PollSet backend (out of scope). */

#include <windows.h>
#include <stdio.h>

#define SER_MAXH 64
static HANDLE ser_handles[SER_MAXH];

static int ser_alloc(HANDLE h) {
    for (int i = 0; i < SER_MAXH; i++) {
        if (ser_handles[i] == NULL || ser_handles[i] == INVALID_HANDLE_VALUE) {
            ser_handles[i] = h;
            return i;
        }
    }
    return -1;
}
static HANDLE ser_h(int fd) {
    if (fd < 0 || fd >= SER_MAXH) return INVALID_HANDLE_VALUE;
    return ser_handles[fd];
}

/* Configure a slot's port as raw 8N1 at `baud` with immediate-return read timeouts. */
int ser_configure(int fd, int baud) {
    HANDLE h = ser_h(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) return -1;
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb)) return -1;
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;      /* return immediately with whatever's buffered */
    to.ReadTotalTimeoutConstant = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(h, &to)) return -1;
    return 0;
}

/* Open a COM port. `path` may be "COM3" (the "\\.\ " prefix is added so COM10+ work).*/
int ser_open(const char *path, int baud) {
    char name[64];
    snprintf(name, sizeof name, "\\\\.\\%s", path);
    HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = ser_alloc(h);
    if (fd < 0) { CloseHandle(h); return -1; }
    if (ser_configure(fd, baud) != 0) { CloseHandle(h); ser_handles[fd] = NULL; return -1; }
    return fd;
}

/* >0 bytes, -1 would-block (nothing buffered), -2 error. */
long ser_read(int fd, char *buf, long cap) {
    HANDLE h = ser_h(fd);
    if (h == INVALID_HANDLE_VALUE) return -2;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)cap, &got, NULL)) return -2;
    return got > 0 ? (long)got : -1;
}

/* >=0 bytes written, -2 error. */
long ser_write(int fd, const char *buf, long n) {
    HANDLE h = ser_h(fd);
    if (h == INVALID_HANDLE_VALUE) return -2;
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD)n, &put, NULL)) return -2;
    return (long)put;
}

int ser_drain(int fd) {
    HANDLE h = ser_h(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return FlushFileBuffers(h) ? 0 : -1;
}
int ser_flush(int fd) {
    HANDLE h = ser_h(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR) ? 0 : -1;
}
void ser_close(int fd) {
    HANDLE h = ser_h(fd);
    if (h != INVALID_HANDLE_VALUE && h != NULL) { CloseHandle(h); ser_handles[fd] = NULL; }
}

#endif
