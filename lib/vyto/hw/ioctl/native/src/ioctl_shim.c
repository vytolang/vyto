/* vyto/hw/ioctl native backing — the generic ioctl escape hatch.

   Most /dev/* devices are driven by ioctl() with device-specific request numbers and C
   structs, not by read()/write(). This package exposes ioctl() generically so a Vyto
   program (or a future pure-Vyto device module) can reach ANY such device without a
   bespoke C shim: pass an fd, a request number, and a byte[] that mirrors the C struct.

   Request numbers are built with the kernel's own _IOC macro (via vt_ioc), so they are
   correct on every architecture — no hardcoded magic. The direction bits come from the
   kernel too. This is the foundation the typed packages (hw/gpio, hw/i2c) encapsulate;
   here it is raw and general.

   Note: ioctl can't do mmap, so devices needing a mapped ring (V4L2 /dev/video*) still
   want a typed package (hw/camera). POSIX. */

#include <stdlib.h>

#ifndef _WIN32
#include <stdint.h>
#include <sys/ioctl.h>
#include <asm/ioctl.h>   /* _IOC, _IOC_NONE/_IOC_READ/_IOC_WRITE */

/* ioctl with a byte-buffer argument (the struct-pointer form). The buffer is written
   in place by read-direction ioctls. Returns the driver's result (>=0), or -1. */
long vt_ioctl_buf(int fd, long req, char *buf) {
    int r = ioctl(fd, (unsigned long)req, buf);
    return (r < 0) ? -1 : (long)r;
}

/* ioctl with a scalar argument passed by value (the _IO form, e.g. enable flags).
   Returns the driver's result (>=0), or -1. */
long vt_ioctl_int(int fd, long req, long val) {
    int r = ioctl(fd, (unsigned long)req, (void *)(intptr_t)val);
    return (r < 0) ? -1 : (long)r;
}

/* Build a request number the arch-correct way, exactly as the kernel headers' _IO/
   _IOR/_IOW/_IOWR macros do. dir is a bitwise OR of the vt_ioc_* direction bits. */
long vt_ioc(int dir, int type, int nr, int size) {
    return (long)(unsigned long)_IOC((unsigned int)dir, (unsigned int)type,
                                     (unsigned int)nr, (unsigned int)size);
}

int vt_ioc_none(void)  { return _IOC_NONE; }
int vt_ioc_read(void)  { return _IOC_READ; }
int vt_ioc_write(void) { return _IOC_WRITE; }

#else /* _WIN32: no POSIX ioctl */

long vt_ioctl_buf(int fd, long req, char *buf) { (void)fd; (void)req; (void)buf; return -1; }
long vt_ioctl_int(int fd, long req, long val) { (void)fd; (void)req; (void)val; return -1; }
long vt_ioc(int dir, int type, int nr, int size) { (void)dir; (void)type; (void)nr; (void)size; return 0; }
int  vt_ioc_none(void)  { return 0; }
int  vt_ioc_read(void)  { return 0; }
int  vt_ioc_write(void) { return 0; }

#endif
