/* vyto/hw/uevent native backing — kernel device events via NETLINK_KOBJECT_UEVENT.

   The push-event source the sysfs packages (hw/power, net/link) lack: the kernel
   broadcasts a uevent whenever a device is added/removed/changed — AC unplugged,
   battery capacity crossed a threshold, cable pulled, USB device attached. It arrives
   on a single poll-able netlink fd, so it folds into a PollSet next to sockets, camera,
   and serial — one wait() covers every hardware change in the system.

   Each message is a datagram of NUL-separated fields: a "ACTION@DEVPATH" header then
   KEY=VALUE lines (SUBSYSTEM, DEVPATH, ACTION, and subsystem-specific keys like
   POWER_SUPPLY_ONLINE / INTERFACE). The shim turns the NULs into newlines so the Vyto
   side can split on "\n" (netlink messages carry no embedded NUL that matters here).

   Receiving kernel uevents on multicast group 1 is normally unprivileged. Non-Linux
   compiles to -1 stubs. */

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>

/* Open a netlink uevent socket subscribed to the kernel's broadcast group, non-blocking.
   Returns the fd, or -1. */
int uevent_open(void) {
    int fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = 0;          /* let the kernel assign a unique pid */
    sa.nl_groups = 1;       /* group 1 = kernel-generated uevents */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

/* Read one uevent datagram, NULs rewritten to '\n', NUL-terminated. >0 bytes, -1
   would-block (nothing pending), -2 error. Read after the fd polls readable. */
long uevent_read(int fd, char *buf, long cap) {
    long n = (long)recv(fd, buf, (size_t)cap - 1, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    for (long i = 0; i < n; i++) if (buf[i] == 0) buf[i] = '\n';
    buf[n] = 0;
    return n;
}

void uevent_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: no netlink uevents */

int  uevent_open(void) { return -1; }
long uevent_read(int fd, char *buf, long cap) { (void)fd; (void)buf; (void)cap; return -2; }
void uevent_close(int fd) { (void)fd; }

#endif
