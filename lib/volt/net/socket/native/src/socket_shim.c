/* volt/net/socket native backing — BSD sockets, blocking.

   A socket is a small int fd, exposed to Volt as i32 and owned by a Volt Socket
   whose deinit calls vsock_close. Blocking with optional SO_*TIMEO timeouts
   (Volt has no threads — the honest gap). Every recv is bounded by the caller's
   bytes(cap); the actual count is returned so Volt never reads uninitialised
   tail bytes. */

#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int vsock_connect(const char *host, int port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int vsock_listen(const char *host, int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!host || !*host || inet_pton(AF_INET, host, &a.sin_addr) != 1)
        a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    if (listen(fd, backlog) != 0) { close(fd); return -1; }
    return fd;
}

int vsock_accept(int fd) { return accept(fd, NULL, NULL); }

int vsock_local_port(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &l) != 0) return -1;
    return ntohs(a.sin_port);
}

long vsock_send(int fd, const char *buf, long n) {
    return (long)send(fd, buf, (size_t)n, MSG_NOSIGNAL);
}
long vsock_recv(int fd, char *buf, long cap) {
    return (long)recv(fd, buf, (size_t)cap, 0);
}

int vsock_set_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int a = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int b = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return (a == 0 && b == 0) ? 0 : -1;
}
int vsock_set_nodelay(int fd, int on) {
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}
void vsock_close(int fd) { if (fd >= 0) close(fd); }

/* ---- UDP ---- */
int vsock_udp_bind(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!host || !*host || inet_pton(AF_INET, host, &a.sin_addr) != 1)
        a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}
long vsock_sendto(int fd, const char *host, int port, const char *buf, long n) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    return (long)sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}
long vsock_recvfrom(int fd, char *buf, long cap) {
    return (long)recvfrom(fd, buf, (size_t)cap, 0, NULL, NULL);
}
