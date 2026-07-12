/* volt/hw/serial native backing — serial / TTY ports over POSIX termios.

   The Volt-way streaming hardware pattern (docs/HARDWARE.md): the port is a fd,
   opened non-blocking so it folds straight into a PollSet event loop next to
   sockets, windows, and workers. Reads/writes are the same encoded-return shape
   as volt/net/socket's try_* calls (>0 bytes, 0 EOF, -1 would-block, -2 error)
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

#else /* _WIN32: COM-port backend not yet implemented */

int  ser_open(const char *path, int baud) { (void)path; (void)baud; return -1; }
int  ser_configure(int fd, int baud) { (void)fd; (void)baud; return -1; }
long ser_read(int fd, char *buf, long cap) { (void)fd; (void)buf; (void)cap; return -2; }
long ser_write(int fd, const char *buf, long n) { (void)fd; (void)buf; (void)n; return -2; }
int  ser_drain(int fd) { (void)fd; return -1; }
int  ser_flush(int fd) { (void)fd; return -1; }
void ser_close(int fd) { (void)fd; }

#endif
