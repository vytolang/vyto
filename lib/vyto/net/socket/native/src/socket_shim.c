/* vyto/net/socket native backing — BSD sockets, blocking.

   A socket is a small int fd, exposed to Vyto as i32 and owned by a Vyto Socket
   whose deinit calls vsock_close. Blocking with optional SO_*TIMEO timeouts
   (Vyto has no threads — the honest gap). Every recv is bounded by the caller's
   bytes(cap); the actual count is returned so Vyto never reads uninitialised
   tail bytes. */

#ifdef _WIN32
/* Before any include: mingw's _mingw.h (reached via <stdio.h>) defaults
   _WIN32_WINNT to the XP-era value, which hides WSAPoll and inet_pton. Both are
   Vista+, which is this port's floor. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>   /* must precede windows.h */
#include <ws2tcpip.h>
#else
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <poll.h>
#endif
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---- platform compatibility ------------------------------------------------

   Winsock is BSD sockets with different spellings, so the bodies below stay
   shared and only the vocabulary is switched here.

   The exported API keeps passing descriptors as plain int, which is what Vyto
   sees. On Win64 a SOCKET is a UINT_PTR, but Microsoft guarantees socket
   handles fit in 32 bits ("Windows Sockets handles... can safely be cast to a
   32-bit value"), so the round trip is lossless. It is also convenient that
   (int)INVALID_SOCKET == -1, which keeps every `fd < 0` failure test below
   working unchanged on both platforms. */

#ifdef _WIN32

typedef SOCKET vsock_t;
#define VS_FD(fd)        ((vsock_t)(intptr_t)(fd))   /* int -> SOCKET */
#define VS_INT(s)        ((int)(intptr_t)(s))        /* SOCKET -> int */
#define vs_closesocket   closesocket
#define vs_lasterr()     WSAGetLastError()
#define VS_EWOULDBLOCK   WSAEWOULDBLOCK
/* A non-blocking connect that has not finished reports WSAEWOULDBLOCK, where
   POSIX reports EINPROGRESS. */
#define VS_EINPROGRESS   WSAEWOULDBLOCK
#define VS_OPTVAL(p)     ((const char *)(p))
#define VS_OPTVAL_MUT(p) ((char *)(p))
typedef int vs_socklen_t;
typedef int vs_iolen_t;                              /* send/recv take int */

/* Winsock needs an explicit per-process init, and getaddrinfo counts. Called at
   the top of every entry point that can be the first socket call in a program.
   Not guarded for concurrency: Vyto has no threads. */
static int vs_startup(void) {
    static int done = 0;
    if (done) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    done = 1;
    return 0;
}

#else

typedef int vsock_t;
#define VS_FD(fd)        (fd)
#define VS_INT(s)        (s)
#define vs_closesocket   close
#define vs_lasterr()     errno
#define VS_EWOULDBLOCK   EWOULDBLOCK
#define VS_EINPROGRESS   EINPROGRESS
#define VS_OPTVAL(p)     (p)
#define VS_OPTVAL_MUT(p) (p)
typedef socklen_t vs_socklen_t;
typedef size_t vs_iolen_t;

static int vs_startup(void) { return 0; }

#endif

/* EAGAIN and EWOULDBLOCK are the same value on Linux but not required to be, so
   test both there; Winsock has only the one. */
static int vs_would_block(int e) {
#ifdef _WIN32
    return e == VS_EWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

int vsock_connect(const char *host, int port) {
    if (vs_startup() != 0) return -1;
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    vsock_t s = VS_FD(-1);
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (VS_INT(s) < 0) continue;
        if (connect(s, rp->ai_addr, (vs_socklen_t)rp->ai_addrlen) == 0) break;
        vs_closesocket(s);
        s = VS_FD(-1);
    }
    freeaddrinfo(res);
    return VS_INT(s);
}

/* reuseport must be set BEFORE bind, so it is a listen parameter rather than
   something a caller can apply to the returned Socket. -1 when reuseport was
   asked for and the platform has no SO_REUSEPORT: a pre-fork server that
   silently got a non-shared socket would look fine and serve on one worker. */
int vsock_listen_ex(const char *host, int port, int backlog, int reuseport) {
    if (vs_startup() != 0) return -1;
    vsock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (VS_INT(s) < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, VS_OPTVAL(&yes), sizeof yes);
    if (reuseport) {
#ifdef SO_REUSEPORT
        if (setsockopt(s, SOL_SOCKET, SO_REUSEPORT, VS_OPTVAL(&yes), sizeof yes) != 0) {
            vs_closesocket(s); return -1;
        }
#else
        vs_closesocket(s); return -1;
#endif
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!host || !*host || inet_pton(AF_INET, host, &a.sin_addr) != 1)
        a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { vs_closesocket(s); return -1; }
    if (listen(s, backlog) != 0) { vs_closesocket(s); return -1; }
    return VS_INT(s);
}

int vsock_listen(const char *host, int port, int backlog) {
    if (vs_startup() != 0) return -1;
    vsock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (VS_INT(s) < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, VS_OPTVAL(&yes), sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!host || !*host || inet_pton(AF_INET, host, &a.sin_addr) != 1)
        a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { vs_closesocket(s); return -1; }
    if (listen(s, backlog) != 0) { vs_closesocket(s); return -1; }
    return VS_INT(s);
}

int vsock_accept(int fd) { return VS_INT(accept(VS_FD(fd), NULL, NULL)); }

int vsock_local_port(int fd) {
    struct sockaddr_in a;
    vs_socklen_t l = sizeof a;
    if (getsockname(VS_FD(fd), (struct sockaddr *)&a, &l) != 0) return -1;
    return ntohs(a.sin_port);
}

long vsock_send(int fd, const char *buf, long n) {
    return (long)send(VS_FD(fd), buf, (vs_iolen_t)n, MSG_NOSIGNAL);
}
long vsock_recv(int fd, char *buf, long cap) {
    return (long)recv(VS_FD(fd), buf, (vs_iolen_t)cap, 0);
}

int vsock_set_timeout(int fd, int ms) {
    /* SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of milliseconds on Winsock, not the
       struct timeval POSIX wants — passing a timeval here silently sets a
       nonsense timeout. */
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#endif
    int a = setsockopt(VS_FD(fd), SOL_SOCKET, SO_RCVTIMEO, VS_OPTVAL(&tv), sizeof tv);
    int b = setsockopt(VS_FD(fd), SOL_SOCKET, SO_SNDTIMEO, VS_OPTVAL(&tv), sizeof tv);
    return (a == 0 && b == 0) ? 0 : -1;
}
int vsock_set_nodelay(int fd, int on) {
    return setsockopt(VS_FD(fd), IPPROTO_TCP, TCP_NODELAY, VS_OPTVAL(&on), sizeof on);
}
/* SO_REUSEPORT: every pre-fork worker opens its own listening socket on the
   same port and the kernel hashes incoming connections across them. Without it
   the workers must share one listen fd, which wakes every worker on every
   connection (the thundering herd). Linux 3.9+ and the BSDs; the constant does
   not exist on Windows, where this returns -1 rather than silently succeeding —
   a caller that needs it must be able to tell. */
int vsock_set_reuseport(int fd, int on) {
#ifdef SO_REUSEPORT
    return setsockopt(VS_FD(fd), SOL_SOCKET, SO_REUSEPORT, VS_OPTVAL(&on), sizeof on);
#else
    (void)fd; (void)on;
    return -1;
#endif
}
void vsock_close(int fd) { if (fd >= 0) vs_closesocket(VS_FD(fd)); }

/* ---- non-blocking (async) --------------------------------------------------

   The blocking send/recv above return -1 on any error, so a would-block is
   indistinguishable from a real failure. The vsock_try_* variants below encode
   the outcome so a poll-driven event loop can tell "come back later" from a
   dead peer. The blocking path is left untouched. */

/* Toggle non-blocking mode on an fd. 0 on success, -1 on error. */
int vsock_set_nonblocking(int fd, int on) {
#ifdef _WIN32
    /* Winsock has no fcntl; FIONBIO is the only way, and it is write-only —
       there is no way to read the current mode back. */
    u_long mode = on ? 1 : 0;
    return ioctlsocket(VS_FD(fd), FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl);
#endif
}

/* >0 = bytes read, 0 = peer closed (EOF), -1 = would-block, -2 = error. */
long vsock_try_recv(int fd, char *buf, long cap) {
    long r = (long)recv(VS_FD(fd), buf, (vs_iolen_t)cap, 0);
    if (r >= 0) return r;
    return vs_would_block(vs_lasterr()) ? -1 : -2;
}

/* >=0 = bytes sent, -1 = would-block, -2 = error. */
/* Send from an offset into a caller-owned buffer. Lets a writer resume a
   partial send without slicing or copying the remainder — the buffer is
   typically a StringBuilder's internal storage, which outlives the call. */
long vsock_try_send_at(int fd, const char *buf, long off, long n) {
    long r = (long)send(VS_FD(fd), buf + off, (vs_iolen_t)n, MSG_NOSIGNAL);
    if (r >= 0) return r;
    return vs_would_block(vs_lasterr()) ? -1 : -2;
}

long vsock_try_send(int fd, const char *buf, long n) {
    long r = (long)send(VS_FD(fd), buf, (vs_iolen_t)n, MSG_NOSIGNAL);
    if (r >= 0) return r;
    return vs_would_block(vs_lasterr()) ? -1 : -2;
}

/* >=0 = new fd (set non-blocking), -1 = would-block, -2 = error. */
int vsock_try_accept(int fd) {
    int c = VS_INT(accept(VS_FD(fd), NULL, NULL));
    if (c >= 0) { vsock_set_nonblocking(c, 1); return c; }
    return vs_would_block(vs_lasterr()) ? -1 : -2;
}

/* Start a non-blocking connect. Returns an O_NONBLOCK fd immediately (connect
   in progress); poll it for POLL_WRITE, then call vsock_conn_result. -1 on
   setup failure. */
int vsock_connect_async(const char *host, int port) {
    if (vs_startup() != 0) return -1;
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    vsock_t s = VS_FD(-1);
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (VS_INT(s) < 0) continue;
        vsock_set_nonblocking(VS_INT(s), 1);
        int rc = connect(s, rp->ai_addr, (vs_socklen_t)rp->ai_addrlen);
        if (rc == 0 || vs_lasterr() == VS_EINPROGRESS) break; /* connected or pending */
        vs_closesocket(s);
        s = VS_FD(-1);
    }
    freeaddrinfo(res);
    return VS_INT(s);
}

/* Result of an async connect once the fd is writable: 0 = connected,
   >0 = error code (failed), -1 = getsockopt error. The non-zero value is an
   errno on POSIX and a WSA error on Windows — callers only test it against 0. */
int vsock_conn_result(int fd) {
    int err = 0;
    vs_socklen_t l = sizeof err;
    if (getsockopt(VS_FD(fd), SOL_SOCKET, SO_ERROR, VS_OPTVAL_MUT(&err), &l) != 0) return -1;
    return err;
}

/* ---- poll set (event loop) -------------------------------------------------

   Opaque poll set so Vyto never marshals a struct pollfd. epoll on Linux,
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
    /* EINTR is NOT an error: a signal arrived while blocked. epoll_wait is
       explicitly non-restartable, so SA_RESTART does not cover it and every
       caller would otherwise see a bare -1 indistinguishable from a real
       failure -- which ends an event loop on the first signal the process
       receives.

       Reported as 0 ready fds, i.e. exactly a timeout. A caller that must know
       a signal landed watches a self-pipe (vyto/os/reactor), which becomes
       readable on the NEXT wait; returning 0 here simply lets the loop go
       round again and see it. */
    if (n < 0) {
        ps->nready = 0;
        return (errno == EINTR) ? 0 : -1;
    }
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

#else  /* poll(2) fallback (macOS, BSD) — and WSAPoll on Windows */

/* WSAPoll is WSAPOLLFD-shaped exactly like struct pollfd and takes the same
   (array, count, timeout_ms) arguments, so Windows rides this branch rather
   than needing a third implementation.

   Two Windows caveats worth knowing, neither fixable here:
   - WSAPoll rejects a zero-length set with WSAEINVAL where poll(2) just sleeps;
     vpoll_wait special-cases that below.
   - WSAPoll does not report a *failed* non-blocking connect: the socket never
     becomes ready rather than signalling POLLERR. A vsock_connect_async to a
     refused port therefore hangs until the caller's own timeout instead of
     surfacing through vsock_conn_result. Microsoft has acknowledged this and
     not fixed it. */
#ifdef _WIN32
typedef WSAPOLLFD vs_pollfd;
typedef ULONG vs_nfds;
#define vs_poll WSAPoll
#else
typedef struct pollfd vs_pollfd;
typedef nfds_t vs_nfds;
#define vs_poll poll
#endif

struct PollSet {
    vs_pollfd *fds;
    int nfds, cap;
    int *ready;      /* indices into fds[] with non-zero revents */
    int nready, rcap;
};

struct PollSet *vpoll_new(void) {
    return (struct PollSet *)calloc(1, sizeof(struct PollSet));
}

int vpoll_mod(struct PollSet *ps, int fd, int events); /* used by vpoll_add below */

static int pollset_find(struct PollSet *ps, int fd) {
    for (int i = 0; i < ps->nfds; i++) if (ps->fds[i].fd == VS_FD(fd)) return i;
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
        vs_pollfd *nf = (vs_pollfd *)realloc(ps->fds, (size_t)want * sizeof *nf);
        if (!nf) return -1;
        ps->fds = nf; ps->cap = want;
    }
    ps->fds[ps->nfds].fd = VS_FD(fd);
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
    ps->nready = 0;
#ifdef _WIN32
    /* WSAPoll fails with WSAEINVAL on an empty set instead of sleeping, which
       would turn an idle event loop into a busy spin returning -1. */
    if (ps->nfds == 0) {
        if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
        return 0;
    }
#endif
    int n = vs_poll(ps->fds, (vs_nfds)ps->nfds, timeout_ms);
    /* Same EINTR rule as the epoll arm above: a signal is not a failure, and
       collapsing it into -1 ends an event loop on the first signal received.
       poll(2) is non-restartable regardless of SA_RESTART. (WSAPoll has no
       EINTR -- Windows has no such delivery -- so this is POSIX-only in
       practice, but the check is harmless there.) */
    if (n < 0) {
#ifndef _WIN32
        if (errno == EINTR) return 0;
#endif
        return -1;
    }
    if (n == 0) return 0;
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
    return VS_INT(ps->fds[ps->ready[i]].fd);
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
    if (vs_startup() != 0) return -1;
    vsock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (VS_INT(s) < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!host || !*host || inet_pton(AF_INET, host, &a.sin_addr) != 1)
        a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { vs_closesocket(s); return -1; }
    return VS_INT(s);
}
long vsock_sendto(int fd, const char *host, int port, const char *buf, long n) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    return (long)sendto(VS_FD(fd), buf, (vs_iolen_t)n, 0, (struct sockaddr *)&a, sizeof a);
}
long vsock_recvfrom(int fd, char *buf, long cap) {
    return (long)recvfrom(VS_FD(fd), buf, (vs_iolen_t)cap, 0, NULL, NULL);
}
