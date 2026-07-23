/* vyto/hw/gpio native backing — GPIO lines via the Linux gpiochip cdev (v2 uAPI).

   The Vyto-way hardware pattern (docs/HARDWARE.md), both halves at once:

     - Handle: open /dev/gpiochipN, request a line, drive it — get/set values through
       the modern GPIO_V2_* ioctls (no sysfs, no deprecated GPIOHANDLE). The chip fd
       and each line-request fd are owned by the Vyto side and closed in deinit.
     - Streaming: an input line requested with an edge flag hands back a poll-able fd;
       edge events (rising/falling) arrive as gpio_v2_line_event records, folding into
       a PollSet exactly like serial/evdev.

   Zero #link, kernel char device. Line access needs the `gpio` group or root (soft:
   denied -> -1). Testable without wiring via the `gpio-sim` kernel module. Non-Linux
   compiles to -1 stubs. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

/* Open a gpiochip (e.g. "/dev/gpiochip0"). Returns the chip fd, or -1. */
int gpio_open(const char *path) {
    return open(path, O_RDWR | O_CLOEXEC);
}

/* Number of lines on the chip, or -1. */
int gpio_chip_lines(int fd) {
    struct gpiochip_info info;
    memset(&info, 0, sizeof info);
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) return -1;
    return (int)info.lines;
}

/* Chip label/name (file-static return buffer; single-threaded). */
static char g_name[32];
const char *gpio_chip_name(int fd) {
    struct gpiochip_info info;
    memset(&info, 0, sizeof info);
    g_name[0] = 0;
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) return g_name;
    memcpy(g_name, info.name, sizeof g_name);
    g_name[sizeof g_name - 1] = 0;
    return g_name;
}

/* Request one line on `chipfd` as output, initial value `value` (0/1). Returns the
   line-request fd (get/set through it), or -1. */
int gpio_request_output(int chipfd, int offset, int value) {
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    req.offsets[0] = (unsigned int)offset;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof req.consumer, "vyto");
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    /* seed the initial output value via a config attribute */
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = value ? 1ULL : 0ULL;
    req.config.attrs[0].mask = 1ULL;
    if (ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) return -1;
    return req.fd;
}

/* Request one line as input. `edge`: 0 none, 1 rising, 2 falling, 3 both — a non-zero
   edge makes the returned fd poll-able for gpio_v2_line_event records. Returns the
   line-request fd, or -1. */
int gpio_request_input(int chipfd, int offset, int edge) {
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    req.offsets[0] = (unsigned int)offset;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof req.consumer, "vyto");
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
    if (edge & 1) req.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
    if (edge & 2) req.config.flags |= GPIO_V2_LINE_FLAG_EDGE_FALLING;
    if (ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) return -1;
    return req.fd;
}

/* Read the current level of a requested line. 0/1, or -1 on error. */
int gpio_get(int linefd) {
    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof v);
    v.mask = 1ULL;
    if (ioctl(linefd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return -1;
    return (v.bits & 1ULL) ? 1 : 0;
}

/* Drive a requested output line to `value` (0/1). 0 ok, -1 error. */
int gpio_set(int linefd, int value) {
    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof v);
    v.mask = 1ULL;
    v.bits = value ? 1ULL : 0ULL;
    return ioctl(linefd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? -1 : 0;
}

/* Read one edge event from an edge-configured input line. Returns 1 (rising) or 2
   (falling), 0 = would-block/EOF, -1 = error. Read after a PollSet reports readable. */
int gpio_event_read(int linefd) {
    struct gpio_v2_line_event ev;
    ssize_t r = read(linefd, &ev, sizeof ev);
    if (r == (ssize_t)sizeof ev) {
        return ev.id == GPIO_V2_LINE_EVENT_RISING_EDGE ? 1 : 2;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return (r < 0) ? -1 : 0;
}

void gpio_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: no gpiochip */

int gpio_open(const char *path) { (void)path; return -1; }
int gpio_chip_lines(int fd) { (void)fd; return -1; }
const char *gpio_chip_name(int fd) { (void)fd; return ""; }
int gpio_request_output(int chipfd, int offset, int value) { (void)chipfd; (void)offset; (void)value; return -1; }
int gpio_request_input(int chipfd, int offset, int edge) { (void)chipfd; (void)offset; (void)edge; return -1; }
int gpio_get(int linefd) { (void)linefd; return -1; }
int gpio_set(int linefd, int value) { (void)linefd; (void)value; return -1; }
int gpio_event_read(int linefd) { (void)linefd; return -1; }
void gpio_close(int fd) { (void)fd; }

#endif
