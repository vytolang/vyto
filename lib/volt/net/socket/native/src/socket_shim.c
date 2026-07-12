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
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <poll.h>
#endif

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

/* ---- non-blocking (async) --------------------------------------------------

   The blocking send/recv above return -1 on any error, so a would-block is
   indistinguishable from a real failure. The vsock_try_* variants below encode
   the outcome so a poll-driven event loop can tell "come back later" from a
   dead peer. The blocking path is left untouched. */

/* Toggle O_NONBLOCK on an fd. 0 on success, -1 on error. */
int vsock_set_nonblocking(int fd, int on) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl);
}

/* >0 = bytes read, 0 = peer closed (EOF), -1 = would-block, -2 = error. */
long vsock_try_recv(int fd, char *buf, long cap) {
    long r = (long)recv(fd, buf, (size_t)cap, 0);
    if (r >= 0) return r;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* >=0 = bytes sent, -1 = would-block, -2 = error. */
long vsock_try_send(int fd, const char *buf, long n) {
    long r = (long)send(fd, buf, (size_t)n, MSG_NOSIGNAL);
    if (r >= 0) return r;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* >=0 = new fd (set O_NONBLOCK), -1 = would-block, -2 = error. */
int vsock_try_accept(int fd) {
    int c = accept(fd, NULL, NULL);
    if (c >= 0) { vsock_set_nonblocking(c, 1); return c; }
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}

/* Start a non-blocking connect. Returns an O_NONBLOCK fd immediately (connect
   in progress); poll it for POLL_WRITE, then call vsock_conn_result. -1 on
   setup failure. */
int vsock_connect_async(const char *host, int port) {
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
        vsock_set_nonblocking(fd, 1);
        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS) break; /* connected or pending */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Result of an async connect once the fd is writable: 0 = connected,
   >0 = errno (failed), -1 = getsockopt error. */
int vsock_conn_result(int fd) {
    int err = 0;
    socklen_t l = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0) return -1;
    return err;
}

/* ---- poll set (event loop) -------------------------------------------------

   Opaque poll set so Volt never marshals a struct pollfd. epoll on Linux,
   poll(2) elsewhere, behind one normalized event-bit contract (VP_*). */

#define VP_READ  1
#define VP_WRITE 2
#define VP_ERR   4   /* HUP/ERR collapsed here */

#ifdef __linux__

struct PollSet {
    int epfd;
    struct epoll_event *ready;
    int nready, rcap;
};

static void pollset_grow_ready(struct PollSet *ps) {
    int want = ps->rcap ? ps->rcap * 2 : 16;
    struct epoll_event *nr = (struct epoll_event *)realloc(ps->ready, (size_t)want * sizeof *nr);
    if (nr) { ps->ready = nr; ps->rcap = want; }
}

struct PollSet *vpoll_new(void) {
    struct PollSet *ps = (struct PollSet *)calloc(1, sizeof *ps);
    if (!ps) return NULL;
    ps->epfd = epoll_create1(0);
    if (ps->epfd < 0) { free(ps); return NULL; }
    return ps;
}

static uint32_t vp_to_epoll(int events) {
    uint32_t e = 0;
    if (events & VP_READ)  e |= EPOLLIN;
    if (events & VP_WRITE) e |= EPOLLOUT;
    return e;
}

int vpoll_add(struct PollSet *ps, int fd, int events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = vp_to_epoll(events);
    ev.data.fd = fd;
    return epoll_ctl(ps->epfd, EPOLL_CTL_ADD, fd, &ev);
}
int vpoll_mod(struct PollSet *ps, int fd, int events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = vp_to_epoll(events);
    ev.data.fd = fd;
    return epoll_ctl(ps->epfd, EPOLL_CTL_MOD, fd, &ev);
}
int vpoll_del(struct PollSet *ps, int fd) {
    return epoll_ctl(ps->epfd, EPOLL_CTL_DEL, fd, NULL);
}

int vpoll_wait(struct PollSet *ps, int timeout_ms) {
    if (ps->rcap == 0) pollset_grow_ready(ps);
    if (ps->rcap == 0) return -1;
    int n = epoll_wait(ps->epfd, ps->ready, ps->rcap, timeout_ms);
    if (n < 0) { ps->nready = 0; return -1; }
    /* if we filled the buffer there may be more; grow for next round */
    if (n == ps->rcap) pollset_grow_ready(ps);
    ps->nready = n;
    return n;
}

int vpoll_ready_fd(struct PollSet *ps, int i) {
    if (i < 0 || i >= ps->nready) return -1;
    return ps->ready[i].data.fd;
}
int vpoll_ready_events(struct PollSet *ps, int i) {
    if (i < 0 || i >= ps->nready) return 0;
    uint32_t e = ps->ready[i].events;
    int out = 0;
    if (e & EPOLLIN)  out |= VP_READ;
    if (e & EPOLLOUT) out |= VP_WRITE;
    if (e & (EPOLLHUP | EPOLLERR)) out |= VP_ERR;
    return out;
}

void vpoll_free(struct PollSet *ps) {
    if (!ps) return;
    if (ps->epfd >= 0) close(ps->epfd);
    free(ps->ready);
    free(ps);
}

#else  /* poll(2) fallback (macOS, BSD) */

struct PollSet {
    struct pollfd *fds;
    int nfds, cap;
    int *ready;      /* indices into fds[] with non-zero revents */
    int nready, rcap;
};

struct PollSet *vpoll_new(void) {
    return (struct PollSet *)calloc(1, sizeof(struct PollSet));
}

static int pollset_find(struct PollSet *ps, int fd) {
    for (int i = 0; i < ps->nfds; i++) if (ps->fds[i].fd == fd) return i;
    return -1;
}
static short vp_to_poll(int events) {
    short e = 0;
    if (events & VP_READ)  e |= POLLIN;
    if (events & VP_WRITE) e |= POLLOUT;
    return e;
}

int vpoll_add(struct PollSet *ps, int fd, int events) {
    if (pollset_find(ps, fd) >= 0) return vpoll_mod(ps, fd, events);
    if (ps->nfds == ps->cap) {
        int want = ps->cap ? ps->cap * 2 : 16;
        struct pollfd *nf = (struct pollfd *)realloc(ps->fds, (size_t)want * sizeof *nf);
        if (!nf) return -1;
        ps->fds = nf; ps->cap = want;
    }
    ps->fds[ps->nfds].fd = fd;
    ps->fds[ps->nfds].events = vp_to_poll(events);
    ps->fds[ps->nfds].revents = 0;
    ps->nfds++;
    return 0;
}
int vpoll_mod(struct PollSet *ps, int fd, int events) {
    int i = pollset_find(ps, fd);
    if (i < 0) return -1;
    ps->fds[i].events = vp_to_poll(events);
    return 0;
}
int vpoll_del(struct PollSet *ps, int fd) {
    int i = pollset_find(ps, fd);
    if (i < 0) return -1;
    ps->fds[i] = ps->fds[ps->nfds - 1]; /* swap-remove */
    ps->nfds--;
    return 0;
}

int vpoll_wait(struct PollSet *ps, int timeout_ms) {
    int n = poll(ps->fds, (nfds_t)ps->nfds, timeout_ms);
    ps->nready = 0;
    if (n <= 0) return n;
    if (ps->rcap < ps->nfds) {
        int *nr = (int *)realloc(ps->ready, (size_t)ps->nfds * sizeof *nr);
        if (!nr) return -1;
        ps->ready = nr; ps->rcap = ps->nfds;
    }
    for (int i = 0; i < ps->nfds && ps->nready < n; i++)
        if (ps->fds[i].revents) ps->ready[ps->nready++] = i;
    return ps->nready;
}

int vpoll_ready_fd(struct PollSet *ps, int i) {
    if (i < 0 || i >= ps->nready) return -1;
    return ps->fds[ps->ready[i]].fd;
}
int vpoll_ready_events(struct PollSet *ps, int i) {
    if (i < 0 || i >= ps->nready) return 0;
    short e = ps->fds[ps->ready[i]].revents;
    int out = 0;
    if (e & POLLIN)  out |= VP_READ;
    if (e & POLLOUT) out |= VP_WRITE;
    if (e & (POLLHUP | POLLERR | POLLNVAL)) out |= VP_ERR;
    return out;
}

void vpoll_free(struct PollSet *ps) {
    if (!ps) return;
    free(ps->fds);
    free(ps->ready);
    free(ps);
}

#endif

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
