/* vyto/os/reactor native backing — the self-pipe wakeup.

   A reactor blocks in poll/epoll on a set of descriptors. Nothing that is not
   one of those descriptors can interrupt it: not a signal, not another thread,
   not another process. The self-pipe is the standard answer — hold both ends of
   a pipe, watch the read end like any other fd, and write one byte to the write
   end to break the wait.

   ── WHY A SIGNAL HANDLER CANNOT DO ANYTHING ELSE ─────────────────────────────

   A signal handler runs between two arbitrary machine instructions of whatever
   the program was doing. Almost nothing is legal there: not malloc (the arena
   lock may be held by the code just interrupted), not printf, not a Vyto
   callback (which allocates, and whose refcounts are non-atomic). POSIX defines
   a short list of async-signal-safe functions, and write(2) is on it.

   So the handler here does exactly one thing: write one byte. The reactor sees
   its read end become readable on its next wakeup and runs the real work on the
   loop's own stack, where everything is legal again. This is the ONLY
   signal-safe shape, which is why it belongs here once rather than in each
   caller.

   errno is saved and restored across the handler. A signal can land between a
   failed syscall and the errno check that follows it in the interrupted code;
   a handler that clobbers errno turns that into a misdiagnosed failure far from
   here.

   ── TRANSPORT ───────────────────────────────────────────────────────────────

   POSIX: pipe2(O_NONBLOCK|O_CLOEXEC) where available, else pipe() plus the
   fcntl calls. Non-blocking matters on the WRITE end: if the pipe fills (many
   wakeups, no drain) a blocking write inside a signal handler would deadlock
   the process against itself. A full pipe already means "wakeup pending", so
   dropping the byte loses nothing.

   Windows has no pipe that WSAPoll can wait on -- WSAPoll takes SOCKETs only.
   So the pair is a loopback TCP socketpair, built by connecting to a listener
   bound to 127.0.0.1:0. Same shape as worker_shim.c's Windows channel, and for
   the same reason.

   The fd is deliberately returned as a plain int rather than a struct across
   the FFI boundary, matching w_spawn's paired-accessor style. */

#ifdef VT_NO_LIBC
/* Freestanding: no OS, no pipes, no signals. --freestanding splices
   -DVT_NO_LIBC into every package shim's compile line, so an unguarded
   <unistd.h> here would break the build of any program that merely imports
   vyto/os/reactor. Keep every symbol so such a program still links, and fail
   as a permission error would: constructors negative, ops negative, drain 0.

   A freestanding reactor still works for timers; it simply has no way to be
   woken from outside, which is the honest answer on a target with no signals.

   Deliberately no #include at all, so this arm cannot regress into depending
   on a hosted header. */

int  vwake_open(int *rfd, int *wfd) { (void)rfd; (void)wfd; return -1; }
int  vwake_signal(int wfd)          { (void)wfd; return -1; }
int  vwake_drain(int rfd)           { (void)rfd; return 0; }
void vwake_close(int rfd, int wfd)  { (void)rfd; (void)wfd; }
int  vwake_on_signal(int signo)     { (void)signo; return -1; }
int  vwake_signal_count(int signo)  { (void)signo; return 0; }

#else /* hosted */

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#endif

#include <string.h>

/* The write end the signal handler writes to. A handler receives only the
   signal number, so this has to be reachable without an argument.

   volatile sig_atomic_t is the only type a handler may touch: it guarantees
   the load/store is a single uninterruptible operation, so the handler can
   never observe a half-written value. -1 means "no reactor is armed", and a
   signal arriving then is dropped rather than written to a stale fd. */
#ifdef _WIN32
static volatile long g_wake_fd = -1;
#else
static volatile sig_atomic_t g_wake_fd = -1;
#endif

/* Per-signal delivery counts. The reactor reads and clears these on the loop's
   own stack after the pipe wakes it, so a Vyto callback never runs in handler
   context. Indexed by signal number; 64 covers every POSIX signal.

   A count rather than a flag: two SIGCHLDs before a drain are two children to
   reap, and collapsing them to one flag loses a child. It saturates rather than
   wrapping -- a counter that wrapped to 0 under a signal storm would report
   "nothing happened" at exactly the busiest moment. */
#define VWAKE_NSIG 64
#ifdef _WIN32
static volatile long g_sigcount[VWAKE_NSIG];
#else
static volatile sig_atomic_t g_sigcount[VWAKE_NSIG];
#endif

#ifndef _WIN32
/* THE handler. Async-signal-safe by construction: one bounded increment and
   one non-blocking write, nothing else.

   The write's result is deliberately ignored (and cast to void to say so): a
   full pipe means a wakeup is already pending and undelivered, which is exactly
   the state this was trying to reach. There is nothing to report and nowhere
   signal-safe to report it to. */
static void vwake_handler(int signo) {
    int saved = errno;
    if (signo >= 0 && signo < VWAKE_NSIG) {
        if (g_sigcount[signo] < 1000000) g_sigcount[signo]++;
    }
    int fd = (int)g_wake_fd;
    if (fd >= 0) {
        char b = 1;
        ssize_t n = write(fd, &b, 1);
        (void)n;
    }
    errno = saved;
}
#endif

/* Create the pair. Writes the read end to *rfd and the write end to *wfd.
   Returns 0, or -1 with both untouched. */
int vwake_open(int *rfd, int *wfd) {
    if (!rfd || !wfd) return -1;

#ifdef _WIN32
    /* WSAPoll waits on SOCKETs only, so the "pipe" is a loopback TCP pair. */
    WSADATA wsa;
    static int inited = 0;
    if (!inited) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        inited = 1;
    }
    SOCKET lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lis == INVALID_SOCKET) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int alen = (int)sizeof a;
    if (bind(lis, (struct sockaddr *)&a, alen) != 0 ||
        listen(lis, 1) != 0 ||
        getsockname(lis, (struct sockaddr *)&a, &alen) != 0) {
        closesocket(lis);
        return -1;
    }

    SOCKET w = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (w == INVALID_SOCKET) { closesocket(lis); return -1; }
    if (connect(w, (struct sockaddr *)&a, alen) != 0) {
        closesocket(w); closesocket(lis);
        return -1;
    }
    SOCKET r = accept(lis, 0, 0);
    closesocket(lis);
    if (r == INVALID_SOCKET) { closesocket(w); return -1; }

    /* Non-blocking on both: same deadlock argument as the POSIX arm. */
    u_long nb = 1;
    ioctlsocket(r, FIONBIO, &nb);
    nb = 1;
    ioctlsocket(w, FIONBIO, &nb);

    *rfd = (int)r;
    *wfd = (int)w;
    g_wake_fd = (long)w;
    return 0;
#else
    int fds[2];
#if defined(__linux__)
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return -1;
#else
    /* No pipe2: create then set the flags. Racy against a concurrent fork+exec
       (the fds are briefly inheritable), which is why pipe2 is preferred where
       it exists. */
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        int fd = fcntl(fds[i], F_GETFD, 0);
        if (fd >= 0) fcntl(fds[i], F_SETFD, fd | FD_CLOEXEC);
    }
#endif
    *rfd = fds[0];
    *wfd = fds[1];
    g_wake_fd = fds[1];
    return 0;
#endif
}

/* Wake the reactor from ordinary (non-handler) code: another thread, or the
   loop itself. One byte; a full pipe is success, since a wakeup is already
   pending. Returns 0 on success or a pending wakeup, -1 on a dead fd. */
int vwake_signal(int wfd) {
    if (wfd < 0) return -1;
    char b = 1;
#ifdef _WIN32
    int n = send((SOCKET)wfd, &b, 1, 0);
    if (n == 1) return 0;
    return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
#else
    ssize_t n = write(wfd, &b, 1);
    if (n == 1) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* already pending */
    if (errno == EINTR) return 0;
    return -1;
#endif
}

/* Drain every queued byte. Returns how many were read.

   Drains in a loop rather than reading one byte: N wakeups may have coalesced,
   and leaving bytes behind would make the fd instantly readable again, spinning
   the loop exactly as an armed-but-empty POLL_WRITE does. */
int vwake_drain(int rfd) {
    if (rfd < 0) return 0;
    char buf[256];
    int total = 0;
    for (;;) {
#ifdef _WIN32
        int n = recv((SOCKET)rfd, buf, (int)sizeof buf, 0);
#else
        ssize_t n = read(rfd, buf, sizeof buf);
#endif
        if (n > 0) {
            total += (int)n;
            if ((size_t)n < sizeof buf) break;   /* drained */
            continue;                            /* full buffer: maybe more */
        }
#ifndef _WIN32
        if (n < 0 && errno == EINTR) continue;
#endif
        break;
    }
    return total;
}

void vwake_close(int rfd, int wfd) {
    /* Disarm the handler's target BEFORE closing, or a signal landing between
       the close and the store would write to a closed (possibly recycled) fd. */
    if ((int)g_wake_fd == wfd) g_wake_fd = -1;
#ifdef _WIN32
    if (rfd >= 0) closesocket((SOCKET)rfd);
    if (wfd >= 0) closesocket((SOCKET)wfd);
#else
    if (rfd >= 0) close(rfd);
    if (wfd >= 0) close(wfd);
#endif
}

/* Route `signo` into the pipe. Returns 0, or -1 if unsupported.

   SA_RESTART is set: an interrupted read(2) elsewhere in the program should
   resume rather than fail with EINTR. The wakeup does not depend on EINTR --
   the pipe write is what breaks the poll -- so there is no reason to make every
   other syscall in the process fallible. */
int vwake_on_signal(int signo) {
    if (signo < 0 || signo >= VWAKE_NSIG) return -1;
#ifdef _WIN32
    /* Windows has no sigaction. signal() exists but delivers on a separate
       thread for most signals and cannot express SA_RESTART; SIGINT via
       SetConsoleCtrlHandler is the real answer and is out of scope here.
       Reported as unsupported rather than half-working. */
    (void)signo;
    return -1;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = vwake_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(signo, &sa, 0) != 0) return -1;
    return 0;
#endif
}

/* Read and clear the delivery count for `signo`. Read-and-clear in one call so
   a count arriving between a peek and a separate clear cannot be lost. */
int vwake_signal_count(int signo) {
    if (signo < 0 || signo >= VWAKE_NSIG) return 0;
    int n = (int)g_sigcount[signo];
    g_sigcount[signo] = 0;
    return n;
}

#endif /* VT_NO_LIBC */
