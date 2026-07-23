/* vyto/hw/i2c native backing — I2C bus master via /dev/i2c-* (I2C_RDWR ioctl).

   The Vyto-way hardware pattern (docs/HARDWARE.md), the handle / byte-transfer half:
   open a bus, then move bytes to/from a 7-bit device address. All transfers go through
   the I2C_RDWR ioctl with explicit i2c_msg lists, so the target address travels with
   each message — no I2C_SLAVE binding, no per-fd state, and combined write-then-read
   (the register-read idiom) is one atomic transaction. The bus fd is owned by the Vyto
   side and closed in deinit.

   Zero #link, kernel char device. Bus access needs the `i2c` group or root (soft:
   denied -> -1). Testable without wiring via the `i2c-stub` kernel module. Non-Linux
   compiles to -1 stubs. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

/* Open an I2C bus (e.g. "/dev/i2c-1"). Returns the fd, or -1. */
int i2c_open(const char *path) {
    return open(path, O_RDWR | O_CLOEXEC);
}

/* Write `len` bytes to 7-bit device `addr`. Bytes written (>=0), or -1 on error. */
long i2c_write(int fd, int addr, const char *buf, long len) {
    struct i2c_msg msg;
    msg.addr = (unsigned short)addr;
    msg.flags = 0;
    msg.len = (unsigned short)len;
    msg.buf = (unsigned char *)buf;
    struct i2c_rdwr_ioctl_data io = { &msg, 1 };
    return ioctl(fd, I2C_RDWR, &io) < 0 ? -1 : len;
}

/* Read `len` bytes from 7-bit device `addr`. Bytes read (>=0), or -1 on error. */
long i2c_read(int fd, int addr, char *buf, long len) {
    struct i2c_msg msg;
    msg.addr = (unsigned short)addr;
    msg.flags = I2C_M_RD;
    msg.len = (unsigned short)len;
    msg.buf = (unsigned char *)buf;
    struct i2c_rdwr_ioctl_data io = { &msg, 1 };
    return ioctl(fd, I2C_RDWR, &io) < 0 ? -1 : len;
}

/* Combined write-then-read to device `addr` in one atomic transaction (the classic
   "write register pointer, read register" idiom): send `wlen` bytes, then read `rlen`
   bytes without releasing the bus. `rlen` bytes read (>=0), or -1 on error. */
long i2c_write_read(int fd, int addr, const char *wbuf, long wlen, char *rbuf, long rlen) {
    struct i2c_msg msgs[2];
    msgs[0].addr = (unsigned short)addr;
    msgs[0].flags = 0;
    msgs[0].len = (unsigned short)wlen;
    msgs[0].buf = (unsigned char *)wbuf;
    msgs[1].addr = (unsigned short)addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = (unsigned short)rlen;
    msgs[1].buf = (unsigned char *)rbuf;
    struct i2c_rdwr_ioctl_data io = { msgs, 2 };
    return ioctl(fd, I2C_RDWR, &io) < 0 ? -1 : rlen;
}

void i2c_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: no I2C */

int  i2c_open(const char *path) { (void)path; return -1; }
long i2c_write(int fd, int addr, const char *buf, long len) { (void)fd; (void)addr; (void)buf; (void)len; return -1; }
long i2c_read(int fd, int addr, char *buf, long len) { (void)fd; (void)addr; (void)buf; (void)len; return -1; }
long i2c_write_read(int fd, int addr, const char *wbuf, long wlen, char *rbuf, long rlen) {
    (void)fd; (void)addr; (void)wbuf; (void)wlen; (void)rbuf; (void)rlen; return -1;
}
void i2c_close(int fd) { (void)fd; }

#endif
