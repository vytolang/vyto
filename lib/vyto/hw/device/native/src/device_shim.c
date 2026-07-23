/* vyto/hw/device native backing — generic character-device byte I/O.

   The lowest common denominator of the Vyto-way (docs/HARDWARE.md): a raw fd with
   read/write/seek, non-blocking so it folds into a PollSet like every other streaming
   device. This reaches ANY device node whose contract is read()/write() — hidraw, tty,
   /dev/random, /dev/cec*, pipes. It deliberately does NOT understand ioctl-driven
   devices (V4L2 /dev/video*, gpiochip, i2c): those need vyto/hw/ioctl or a typed
   package. Same encoded returns as serial/socket. POSIX; Windows is a later backend. */

#define _GNU_SOURCE
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Open `path`. mode: 0 read, 1 write, 2 read-write, 3 write+create+truncate (for
   writing a new file, e.g. a recording); nonblock adds O_NONBLOCK. Returns the fd, or
   -1 (missing node, no permission). Mode 3 creates the file 0644 if absent. */
int dev_open(const char *path, int mode, int nonblock) {
    int flags = (mode == 1) ? O_WRONLY
              : (mode == 2) ? O_RDWR
              : (mode == 3) ? (O_WRONLY | O_CREAT | O_TRUNC)
              : O_RDONLY;
    if (nonblock) flags |= O_NONBLOCK;
    return open(path, flags, 0644);
}

/* >0 = bytes read, 0 = EOF, -1 = would-block, -2 = error. */
long dev_read(int fd, char *buf, long cap) {
    long r = (long)read(fd, buf, (size_t)cap);
    if (r >= 0) return r;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* >=0 = bytes written, -1 = would-block, -2 = error. */
long dev_write(int fd, const char *buf, long n) {
    long w = (long)write(fd, buf, (size_t)n);
    if (w >= 0) return w;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* Reposition. whence: 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END. New offset, or -1. */
long dev_seek(int fd, long off, int whence) {
    off_t r = lseek(fd, (off_t)off, whence);
    return (r == (off_t)-1) ? -1 : (long)r;
}

void dev_close(int fd) { if (fd >= 0) close(fd); }

#else /* _WIN32: generic device I/O not yet implemented */

int  dev_open(const char *path, int mode, int nonblock) { (void)path; (void)mode; (void)nonblock; return -1; }
long dev_read(int fd, char *buf, long cap) { (void)fd; (void)buf; (void)cap; return -2; }
long dev_write(int fd, const char *buf, long n) { (void)fd; (void)buf; (void)n; return -2; }
long dev_seek(int fd, long off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
void dev_close(int fd) { (void)fd; }

#endif
