/* vyto/net/raw native backing — link-layer frames via AF_PACKET raw sockets.

   Below IP: sniff and inject whole Ethernet frames (headers included) on an interface.
   This is the packet-analyzer / custom-L2-protocol layer — the fd folds into a PollSet
   like every other stream, and frames cross as byte[]. It is a socket type, so it lives
   under net/ (reusing the socket discipline), not hw/.

   Opening an AF_PACKET socket requires CAP_NET_RAW (root, or the capability), so a
   denied open is soft (-1). Encoded returns match vyto/net/socket. Linux only (BSD uses
   BPF; a later backend). */

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <arpa/inet.h>

/* Open a raw packet socket bound to `iface` (all EtherTypes), non-blocking. Returns the
   fd, or -1 (no CAP_NET_RAW, or unknown interface). */
int raw_open(const char *iface) {
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { close(fd); return -1; }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) { close(fd); return -1; }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

/* >0 = bytes of a received frame, 0 = none, -1 = would-block, -2 = error. */
long raw_recv(int fd, char *buf, long cap) {
    long r = (long)recv(fd, buf, (size_t)cap, 0);
    if (r > 0) return r;
    if (r == 0) return 0;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* Inject a complete frame (with its own Ethernet header). >=0 sent, -1 would-block,
   -2 error. */
long raw_send(int fd, const char *buf, long n) {
    long w = (long)send(fd, buf, (size_t)n, 0);
    if (w >= 0) return w;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

void raw_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: no AF_PACKET */

int  raw_open(const char *iface) { (void)iface; return -1; }
long raw_recv(int fd, char *buf, long cap) { (void)fd; (void)buf; (void)cap; return -2; }
long raw_send(int fd, const char *buf, long n) { (void)fd; (void)buf; (void)n; return -2; }
void raw_close(int fd) { (void)fd; }

#endif
