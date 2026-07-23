/* vyto/hw/camera native backing — webcam capture via V4L2 (mmap streaming).

   The Vyto-way hardware pattern (docs/HARDWARE.md) at its fullest: a camera is BOTH a
   poll-able fd (frame-ready) AND a byte[] producer (the frame). V4L2 can't be driven by
   plain read() — it needs VIDIOC_* ioctls plus an mmap ring — which is exactly why this
   is a typed package rather than something vyto/hw/device or vyto/hw/ioctl can do.

   Flow: open /dev/videoN non-blocking, negotiate a YUYV format at the requested size,
   VIDIOC_REQBUFS an mmap ring, map + queue the buffers, VIDIOC_STREAMON. Each frame:
   the fd becomes readable (folds into a PollSet), VIDIOC_DQBUF hands back a filled
   buffer, we copy it out and VIDIOC_QBUF it back. deinit STREAMOFFs, unmaps, closes.

   Uncompressed YUYV only (the common webcam format); MJPEG-only devices report their
   fourcc so the caller can tell. A YUYV->RGBA helper produces blit-ready pixels. Zero
   #link. Non-Linux compiles to null/stubs. */

#include <stdlib.h>
#include <string.h>

typedef struct {
    int fd;
    int width, height;
    unsigned int fourcc;
    unsigned int sizeimage;
    void *buf_start[8];
    unsigned int buf_len[8];
    int nbufs;
} Camera;

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

void cam_close(Camera *c); /* forward decl: cam_open's error paths unwind through it */

/* Retry an ioctl across EINTR. */
static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/* Open /dev/videoN, negotiate YUYV at ~req_w x req_h, set up and start an mmap ring.
   Returns an opaque Camera*, or NULL (missing device, no permission, not a capture/
   streaming device). The negotiated size/format may differ from the request; read it
   back with cam_width/height/fourcc. */
Camera *cam_open(const char *path, int req_w, int req_h) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return NULL;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        close(fd); return NULL;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (unsigned)req_w;
    fmt.fmt.pix.height = (unsigned)req_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { close(fd); return NULL; }

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1) { close(fd); return NULL; }

    Camera *c = (Camera *)calloc(1, sizeof *c);
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    c->width = (int)fmt.fmt.pix.width;
    c->height = (int)fmt.fmt.pix.height;
    c->fourcc = fmt.fmt.pix.pixelformat;
    c->sizeimage = fmt.fmt.pix.sizeimage;
    c->nbufs = (int)rb.count;
    if (c->nbufs > 8) c->nbufs = 8;

    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned)i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { cam_close(c); return NULL; }
        c->buf_len[i] = b.length;
        c->buf_start[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (c->buf_start[i] == MAP_FAILED) { c->buf_start[i] = NULL; cam_close(c); return NULL; }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { cam_close(c); return NULL; }
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) { cam_close(c); return NULL; }
    return c;
}

int  cam_fd(Camera *c)     { return c ? c->fd : -1; }
int  cam_width(Camera *c)  { return c ? c->width : 0; }
int  cam_height(Camera *c) { return c ? c->height : 0; }
int  cam_fourcc(Camera *c) { return c ? (int)c->fourcc : 0; }
long cam_frame_size(Camera *c) { return c ? (long)c->sizeimage : 0; }

/* Dequeue one frame, copy up to `cap` bytes into dst, requeue. >=0 bytes copied,
   -1 = no frame ready (would-block), -2 = error. Non-blocking; call after the fd polls
   readable. */
long cam_grab(Camera *c, char *dst, long cap) {
    if (!c) return -2;
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0)
        return (errno == EAGAIN) ? -1 : -2;
    long n = (long)b.bytesused;
    if (n > cap) n = cap;
    if (b.index < (unsigned)c->nbufs && c->buf_start[b.index])
        memcpy(dst, c->buf_start[b.index], (size_t)n);
    xioctl(c->fd, VIDIOC_QBUF, &b); /* requeue for reuse */
    return n;
}

void cam_close(Camera *c) {
    if (!c) return;
    if (c->fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(c->fd, VIDIOC_STREAMOFF, &t);
    }
    for (int i = 0; i < c->nbufs; i++)
        if (c->buf_start[i]) munmap(c->buf_start[i], c->buf_len[i]);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

#else /* non-Linux: no V4L2 */

Camera *cam_open(const char *path, int req_w, int req_h) { (void)path; (void)req_w; (void)req_h; return NULL; }
int  cam_fd(Camera *c)     { (void)c; return -1; }
int  cam_width(Camera *c)  { (void)c; return 0; }
int  cam_height(Camera *c) { (void)c; return 0; }
int  cam_fourcc(Camera *c) { (void)c; return 0; }
long cam_frame_size(Camera *c) { (void)c; return 0; }
long cam_grab(Camera *c, char *dst, long cap) { (void)c; (void)dst; (void)cap; return -2; }
void cam_close(Camera *c) { (void)c; }

#endif

/* Clamp an int to a byte. */
static unsigned char clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)v; }

/* Convert a YUYV frame (w*h*2 bytes) to RGBA (w*h*4 bytes), BT.601. Platform-neutral —
   pure arithmetic, no syscalls — so it lives outside the #ifdef. `dst` must hold at
   least w*h*4 bytes. */
void cam_yuyv_to_rgba(const char *src, int w, int h, char *dst) {
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;
    int pairs = (w * h) / 2;
    for (int i = 0; i < pairs; i++) {
        int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
        s += 4;
        int d0 = u - 128, e0 = v - 128;
        int c0 = 298 * (y0 - 16), c1 = 298 * (y1 - 16);
        /* pixel 0 */
        d[0] = clamp8((c0 + 409 * e0 + 128) >> 8);
        d[1] = clamp8((c0 - 100 * d0 - 208 * e0 + 128) >> 8);
        d[2] = clamp8((c0 + 516 * d0 + 128) >> 8);
        d[3] = 255;
        /* pixel 1 */
        d[4] = clamp8((c1 + 409 * e0 + 128) >> 8);
        d[5] = clamp8((c1 - 100 * d0 - 208 * e0 + 128) >> 8);
        d[6] = clamp8((c1 + 516 * d0 + 128) >> 8);
        d[7] = 255;
        d += 8;
    }
}
