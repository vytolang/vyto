/* vyto/hw/ioctl native backing — the generic ioctl escape hatch.

   Most device nodes are driven by ioctl() with device-specific request numbers and C
   structs, not by read()/write(). This package exposes ioctl() generically so a Vyto
   program (or a future pure-Vyto device module) can reach ANY such device without a
   bespoke C shim: pass an fd, a request number, and a byte[] that mirrors the C struct.

   Request numbers are built with the kernel's own _IOC macro (via vt_ioc), so they are
   correct on every architecture — no hardcoded magic. The direction bits come from the
   kernel too. This is the foundation the typed packages (hw/gpio, hw/i2c) encapsulate;
   here it is raw and general.

   Note: ioctl can't do mmap, so devices needing a mapped ring (V4L2 /dev/video*) still
   want a typed package (hw/camera).

   Windows: DeviceIoControl on the HANDLE that vyto/hw/device's rawFd() returns,
   with the byte[] as both input and output buffer (the in-place shape a POSIX ioctl
   struct has). Request numbers there are CTL_CODEs (ctlCode in ioctl.vt); the _IOC
   builders return 0. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#include <stdint.h>
#include <stdlib.h>

#if !defined(_WIN32) && !defined(VT_NO_LIBC)
#include <sys/ioctl.h>
#include <asm/ioctl.h>   /* _IOC, _IOC_NONE/_IOC_READ/_IOC_WRITE */

/* ioctl with a byte-buffer argument (the struct-pointer form). The buffer is written
   in place by read-direction ioctls. Returns the driver's result (>=0), or -1. */
int64_t vt_ioctl_buf(int64_t fd, int64_t req, char *buf, int64_t len) {
    (void)len;
    int r = ioctl((int)fd, (unsigned long)req, buf);
    return (r < 0) ? -1 : (int64_t)r;
}

/* ioctl with a scalar argument passed by value (the _IO form, e.g. enable flags).
   Returns the driver's result (>=0), or -1. */
int64_t vt_ioctl_int(int64_t fd, int64_t req, int64_t val) {
    int r = ioctl((int)fd, (unsigned long)req, (void *)(intptr_t)val);
    return (r < 0) ? -1 : (int64_t)r;
}

/* Build a request number the arch-correct way, exactly as the kernel headers' _IO/
   _IOR/_IOW/_IOWR macros do. dir is a bitwise OR of the vt_ioc_* direction bits. */
int64_t vt_ioc(int dir, int type, int nr, int size) {
    return (int64_t)(unsigned long)_IOC((unsigned int)dir, (unsigned int)type,
                                     (unsigned int)nr, (unsigned int)size);
}

int vt_ioc_none(void)  { return _IOC_NONE; }
int vt_ioc_read(void)  { return _IOC_READ; }
int vt_ioc_write(void) { return _IOC_WRITE; }

#elif !defined(VT_NO_LIBC) /* _WIN32 */
#include <windows.h>

/* One control call on `fd` (a HANDLE's value). An OVERLAPPED is always passed:
   Windows ignores it for a handle opened synchronously and needs it for an
   overlapped one (a non-blocking vyto/hw/device open), and either way the call
   is waited for here. Bytes the driver returned (>= 0), or -1. */
static int64_t win_ioctl(int64_t fd, int64_t req, void *in, DWORD inlen, void *out,
                         DWORD outlen) {
    HANDLE h = (HANDLE)(intptr_t)fd;
    if (!h || h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;
    DWORD got = 0;
    int64_t r = -1;
    if (DeviceIoControl(h, (DWORD)req, in, inlen, out, outlen, &got, &ov) ||
        GetLastError() == ERROR_IO_PENDING) {
        if (GetOverlappedResult(h, &ov, &got, TRUE)) r = (int64_t)got;
    }
    CloseHandle(ov.hEvent);
    return r;
}

int64_t vt_ioctl_buf(int64_t fd, int64_t req, char *buf, int64_t len) {
    DWORD n = len > 0 && len < 0x7fffffff ? (DWORD)len : 0;
    return win_ioctl(fd, req, n ? buf : NULL, n, n ? buf : NULL, n);
}

/* The scalar form: the value goes in as a 4-byte input, nothing comes out
   (0 sends no input at all). */
int64_t vt_ioctl_int(int64_t fd, int64_t req, int64_t val) {
    int32_t v = (int32_t)val;
    return win_ioctl(fd, req, val ? &v : NULL, val ? sizeof v : 0, NULL, 0);
}

int64_t vt_ioc(int dir, int type, int nr, int size) { (void)dir; (void)type; (void)nr; (void)size; return 0; }
int  vt_ioc_none(void)  { return 0; }
int  vt_ioc_read(void)  { return 0; }
int  vt_ioc_write(void) { return 0; }

#else /* freestanding */

int64_t vt_ioctl_buf(int64_t fd, int64_t req, char *buf, int64_t len) { (void)fd; (void)req; (void)buf; (void)len; return -1; }
int64_t vt_ioctl_int(int64_t fd, int64_t req, int64_t val) { (void)fd; (void)req; (void)val; return -1; }
int64_t vt_ioc(int dir, int type, int nr, int size) { (void)dir; (void)type; (void)nr; (void)size; return 0; }
int  vt_ioc_none(void)  { return 0; }
int  vt_ioc_read(void)  { return 0; }
int  vt_ioc_write(void) { return 0; }

#endif
