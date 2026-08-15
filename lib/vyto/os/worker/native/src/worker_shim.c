/* vyto/os/worker native backing — a worker pool of real OS processes.

   POSIX: pre-fork over a connected AF_UNIX socketpair. fork() gives real CPU
   parallelism with no shared state: each child is a full copy of the process,
   including the job closure, and talks to the parent over its socket.

   Windows: there is no fork, so a child cannot inherit the closure. Instead the
   worker is the *same executable re-launched* (CreateProcess with this process's
   own command line) with VYTO_WORKER_* set in its environment. It re-runs main,
   reaches the same `new WorkerPool(n, job)` call, and w_worker_channel() tells
   it at that point to serve jobs on its channel instead of building a pool of
   its own — so it arrives holding an identically-constructed closure. This is
   the same trade Python's multiprocessing "spawn" start method makes, and it
   carries the same caveat: anything main() does before constructing the pool
   happens once per worker too, so side effects belong after it.

   The channel is a 127.0.0.1 socket rather than an inherited handle, which
   keeps handle-inheritance out of it and lets the frame protocol below run
   unchanged over both transports.

   Messages are length-prefixed frames (4-byte big-endian length + payload)
   written/read in full by the loops below, so a job's input and result never
   tear across a partial read. The parent never packs a struct across FFI: the
   fd comes back from w_spawn as a plain int and the pid via a paired accessor,
   which is safe because the parent spawns sequentially.

   Every worker gets a SECOND channel beside the data one: the control channel.
   The data channel is strictly request/response and a busy worker is, by
   definition, not reading it — so there is no way to reach a worker mid-job over
   it, which is what cancelling one requires. The control channel carries opaque
   bytes in both directions and is never part of the job protocol: the parent can
   write to it while a job runs, and the child can hand back out-of-band state
   (a backend pid, a cancel key) the instant it has it rather than at the end.

   On POSIX that is a second socketpair from the same fork. On Windows the child
   connects back to the parent's listener twice, data first, and both connections
   present the token. */

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <process.h>
#else
#define _GNU_SOURCE
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#endif

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ---- transport compatibility ---------------------------------------------
   Same int-fd contract on both sides; on Windows the int is a SOCKET, which
   Microsoft guarantees fits in 32 bits. */

#ifdef _WIN32
#define W_FD(fd)   ((SOCKET)(intptr_t)(fd))
#define W_INT(s)   ((int)(intptr_t)(s))
static int w_read(int fd, char *p, size_t n)  { return recv(W_FD(fd), p, (int)n, 0); }
static int w_write(int fd, const char *p, size_t n) { return send(W_FD(fd), p, (int)n, 0); }
static void w_closefd(int fd) { closesocket(W_FD(fd)); }
#else
#define W_FD(fd)   (fd)
#define W_INT(s)   (s)
static int w_read(int fd, char *p, size_t n)  { return (int)read(fd, p, n); }
static int w_write(int fd, const char *p, size_t n) { return (int)write(fd, p, n); }
static void w_closefd(int fd) { close(fd); }
#endif

void w_close(int fd) { if (fd >= 0) w_closefd(fd); }

/* _exit (not exit) in the worker: skip the parent's atexit/stdio-flush
   handlers, which would otherwise double-flush buffers inherited by the fork. */
void w_exit(int code) { _exit(code); }

/* A signal arriving mid-transfer must not look like a broken worker: EINTR is a
   "try again", not an error, and these loops are the only thing standing between
   a stray signal and a retired process. */
static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        int w = w_write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
/* 0 = filled, 1 = clean EOF, -1 = error. */
static int read_all(int fd, char *p, size_t n) {
    while (n) {
        int r = w_read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) return 1;
        if (r < 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* The largest frame either side will write or accept. The wire length is a
   uint32 but every reader here returns it as an int, so anything at or above
   2^31 would come back negative and be misread as EOF. Capping well below that
   turns a corrupt or hostile length into a clean error instead of a bogus
   allocation. 1 GiB is far past any real job payload. */
#define W_MAX_FRAME (1 << 30)

/* Wait for `fd` to become readable. 1 = readable, 0 = timed out, -1 = error.
   timeoutMs < 0 blocks forever, 0 polls. Used to check a control channel
   without committing to a read that would block. */
int w_poll_readable(int fd, int timeoutMs) {
    if (fd < 0) return -1;
#ifdef _WIN32
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(W_FD(fd), &rf);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = select(0, &rf, NULL, NULL, timeoutMs < 0 ? NULL : &tv);
#else
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int rc;
    do { rc = poll(&p, 1, timeoutMs); } while (rc < 0 && errno == EINTR);
#endif
    if (rc < 0) return -1;
    return rc > 0 ? 1 : 0;
}

/* Write a length-prefixed frame. 0 ok, -1 error. */
int w_write_frame(int fd, const char *buf, int n) {
    if (n < 0) n = 0;
    if (n > W_MAX_FRAME) return -1;
    uint32_t len = (uint32_t)n;
    unsigned char hdr[4] = { (unsigned char)(len >> 24), (unsigned char)(len >> 16),
                             (unsigned char)(len >> 8), (unsigned char)len };
    if (write_all(fd, (const char *)hdr, 4) != 0) return -1;
    if (n > 0 && write_all(fd, buf, (size_t)n) != 0) return -1;
    return 0;
}

/* Read the next frame's length. >=0 length, -1 = peer closed (EOF), -2 = error.
   A length past W_MAX_FRAME is reported as an error rather than returned: the
   caller would otherwise allocate on a number it never sanity-checked. */
int w_read_len(int fd) {
    unsigned char hdr[4];
    int r = read_all(fd, (char *)hdr, 4);
    if (r == 1) return -1;
    if (r < 0) return -2;
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (len > (uint32_t)W_MAX_FRAME) return -2;
    return (int)len;
}
/* Read `n` payload bytes into buf (call right after w_read_len). 0 ok, -1 error. */
int w_read_bytes(int fd, char *buf, int n) {
    if (n <= 0) return 0;
    return read_all(fd, buf, (size_t)n) == 0 ? 0 : -1;
}

static int g_last_pid = -1;
int w_last_pid(void) { return g_last_pid; }

/* The parent-side control fd of the worker w_spawn just created. Paired
   accessor, same contract as w_last_pid: valid until the next w_spawn, which is
   safe because the parent spawns sequentially. */
static int g_last_ctl = -1;
int w_last_ctl_fd(void) { return g_last_ctl; }

/* ===================== POSIX: fork ======================================== */
#ifndef _WIN32

static int g_channel = -1;
static int g_ctl_channel = -1;
int w_worker_channel(void) { return g_channel; }
int w_worker_control(void) { return g_ctl_channel; }

/* Spawn one worker. In the parent returns the parent-side data fd (or -1 on
   failure), with its control fd at w_last_ctl_fd(); in the forked child returns
   0, with both fds available from w_worker_channel()/w_worker_control().
   `slot` is unused here — fork needs no addressing. */
int w_spawn(int slot) {
    (void)slot;
    int sv[2], cv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cv) != 0) {
        close(sv[0]); close(sv[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]); close(cv[0]); close(cv[1]);
        return -1;
    }
    if (pid == 0) {
        close(sv[0]); close(cv[0]);   /* child keeps only its own ends */
        g_channel = sv[1];
        g_ctl_channel = cv[1];
        return 0;
    }
    close(sv[1]); close(cv[1]);       /* parent keeps only its own ends */
    g_last_pid = (int)pid;
    g_last_ctl = cv[0];
    return sv[0];
}

/* Terminate and reap a worker. Closing the parent fd already makes the child
   exit on EOF; the SIGTERM covers a child wedged in a long job. */
void w_reap(int pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

/* ===================== Windows: re-exec =================================== */
#else

#define W_TOKEN_HEX 32          /* 16 random bytes, hex-encoded */
#define W_MAX_WORKERS 256

static HANDLE g_procs[W_MAX_WORKERS];
static int g_nprocs = 0;

static int wsa_ready(void) {
    static int done = 0;
    if (done) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    done = 1;
    return 0;
}

/* Hex-encode n cryptographically random bytes into out (2n+1 chars). The token
   authenticates the child on a loopback port that any local process could
   otherwise connect to first. */
static int w_token(char *out, int n) {
    unsigned char raw[32];
    if (n > (int)sizeof raw) n = (int)sizeof raw;
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, raw, (ULONG)n,
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return -1;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[n * 2] = 0;
    return 0;
}

/* One connection back to the parent's listener, authenticated with the token.
   INVALID_SOCKET on any failure. */
static SOCKET w_connect_back(const char *port, const char *tok) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    if (send(s, tok, W_TOKEN_HEX, 0) != W_TOKEN_HEX) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static int g_ctl_channel = -1;
int w_worker_control(void) { return g_ctl_channel; }

/* This process's channels, if it was launched as a worker. Resolved once, on
   first call: connect back to the parent's loopback port twice — data first,
   then control — proving identity with the token each time. The parent accepts
   in the same order, which is what pairs the two. -1 means "not a worker" (the
   normal case). */
int w_worker_channel(void) {
    static int ch = -2;                 /* -2 = not yet resolved */
    if (ch != -2) return ch;
    ch = -1;

    const char *port = getenv("VYTO_WORKER_PORT");
    const char *tok  = getenv("VYTO_WORKER_TOKEN");
    if (!port || !*port || !tok || !*tok) return ch;
    if (wsa_ready() != 0) return ch;

    SOCKET d = w_connect_back(port, tok);
    if (d == INVALID_SOCKET) return ch;
    SOCKET c = w_connect_back(port, tok);
    if (c == INVALID_SOCKET) { closesocket(d); return ch; }
    g_ctl_channel = W_INT(c);
    ch = W_INT(d);
    return ch;
}

/* First free slot in the process table, appending if none was freed. Respawn
   makes reuse matter: without it a long-running parent that replaces workers
   walks g_nprocs to W_MAX_WORKERS and then cannot spawn at all. -1 if full.
   Side-effect free — the caller commits by writing g_procs[i] and, when the slot
   was an append, bumping g_nprocs — so it doubles as a "would this fit?" probe. */
static int w_proc_slot(void) {
    for (int i = 0; i < g_nprocs; i++) { if (!g_procs[i]) return i; }
    if (g_nprocs >= W_MAX_WORKERS) return -1;
    return g_nprocs;
}

/* Accept one connection and require the token, so a local process that races the
   child onto the port cannot become our worker. INVALID_SOCKET on failure. */
static SOCKET w_accept_auth(SOCKET lsn, const char *tok) {
    SOCKET c = accept(lsn, NULL, NULL);
    if (c == INVALID_SOCKET) return INVALID_SOCKET;
    char got[W_TOKEN_HEX];
    if (read_all(W_INT(c), got, W_TOKEN_HEX) != 0 || memcmp(got, tok, W_TOKEN_HEX) != 0) {
        closesocket(c);
        return INVALID_SOCKET;
    }
    return c;
}

/* Spawn one worker process: listen on an ephemeral loopback port, re-launch
   this executable with the port and token in its environment, then accept its
   two connections back (data, then control). Returns the parent-side data fd
   with the control fd at w_last_ctl_fd(), or -1. Never returns 0 — there is no
   child return path on Windows. */
int w_spawn(int slot) {
    if (wsa_ready() != 0) return -1;
    if (w_proc_slot() < 0) return -1;

    SOCKET lsn = socket(AF_INET, SOCK_STREAM, 0);
    if (lsn == INVALID_SOCKET) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = 0;                              /* ephemeral */
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* loopback only, never ANY */
    int fd = -1;
    char tok[W_TOKEN_HEX + 1];
    if (bind(lsn, (struct sockaddr *)&a, sizeof a) != 0) goto done;
    if (listen(lsn, 2) != 0) goto done;          /* data + control */

    int alen = sizeof a;
    if (getsockname(lsn, (struct sockaddr *)&a, &alen) != 0) goto done;
    if (w_token(tok, W_TOKEN_HEX / 2) != 0) goto done;

    char portbuf[16], slotbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", (int)ntohs(a.sin_port));
    snprintf(slotbuf, sizeof slotbuf, "%d", slot);

    /* Inherited by the child via the default (NULL) environment block. Spawning
       is sequential, so a single set/clear pair per worker is safe. */
    SetEnvironmentVariableA("VYTO_WORKER_SLOT", slotbuf);
    SetEnvironmentVariableA("VYTO_WORKER_PORT", portbuf);
    SetEnvironmentVariableA("VYTO_WORKER_TOKEN", tok);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    /* Re-launch with this process's own image and command line, so the child
       reaches the identical WorkerPool construction. GetCommandLineW is not
       const — CreateProcessW may write to it — so it is copied first. */
    WCHAR exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) goto unset;
    LPWSTR cl = GetCommandLineW();
    size_t cln = wcslen(cl) + 1;
    WCHAR *cmd = (WCHAR *)malloc(cln * sizeof(WCHAR));
    if (!cmd) goto unset;
    memcpy(cmd, cl, cln * sizeof(WCHAR));

    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) goto unset;
    CloseHandle(pi.hThread);

    /* Two authenticated connections, in the order the child makes them: data
       first, then control. Anything short of both is a half-built worker, so it
       is torn down rather than handed back. */
    SOCKET c = w_accept_auth(lsn, tok);
    if (c == INVALID_SOCKET) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); goto unset; }
    SOCKET cc = w_accept_auth(lsn, tok);
    if (cc == INVALID_SOCKET) {
        closesocket(c);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        goto unset;
    }

    int ps = w_proc_slot();
    if (ps < 0) {
        closesocket(c); closesocket(cc);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        goto unset;
    }
    g_procs[ps] = pi.hProcess;
    if (ps == g_nprocs) g_nprocs++;
    g_last_pid = ps + 1;       /* 1-based index, so 0 never looks like a pid */
    g_last_ctl = W_INT(cc);
    fd = W_INT(c);

unset:
    SetEnvironmentVariableA("VYTO_WORKER_SLOT", NULL);
    SetEnvironmentVariableA("VYTO_WORKER_PORT", NULL);
    SetEnvironmentVariableA("VYTO_WORKER_TOKEN", NULL);
done:
    closesocket(lsn);
    return fd;
}

/* `pid` here is the 1-based index handed out by w_spawn, not an OS pid. */
void w_reap(int pid) {
    if (pid <= 0 || pid > g_nprocs) return;
    HANDLE h = g_procs[pid - 1];
    if (!h) return;
    /* Closing the parent socket already makes the worker exit on EOF; give it a
       moment before killing, to match the POSIX SIGTERM-then-wait shape. */
    if (WaitForSingleObject(h, 200) == WAIT_TIMEOUT) TerminateProcess(h, 0);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    g_procs[pid - 1] = NULL;
}

#endif
