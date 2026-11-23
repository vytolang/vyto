/* vyto/os native backing — starting a program with an argument list, no
   shell in between.

   `os_run` goes through the shell, which is right for a command line a person
   typed and wrong for a path a program computed: a space or a quote in the
   path changes what runs. These take the executable as a path and each
   argument as its own string.

     os_spawn_detached  start it and let it go: it outlives this process,
                        holds none of our stdio, and is never our zombie
     os_run_args        start it, wait for it, return its exit code
     os_pid_alive       is any process with this id still running
     os_wait_pid_exit   wait until it is not

   The argument list is built through a small handle (os_argv_new/add/free),
   so no separator character is reserved. argv[0] is the executable itself;
   the caller adds only the arguments.

   A self-updater is the first user: the running app starts a copy of itself
   detached, exits, and the copy waits for it to be gone (os_wait_pid_exit)
   before replacing the install and running setup (os_run_args). */

#include <stdint.h>

#ifdef VT_NO_LIBC

/* No processes without an OS. Every entry point returns its documented
   failure sentinel so the .vt needs no platform conditional. */
void *os_argv_new(void) { return 0; }
void os_argv_add(void *h, const char *s) { (void)h; (void)s; }
void os_argv_free(void *h) { (void)h; }
int os_spawn_detached(const char *exe, void *h) { (void)exe; (void)h; return -1; }
int os_run_args(const char *exe, void *h, int ms) { (void)exe; (void)h; (void)ms; return -1; }
int os_pid_alive(int pid) { (void)pid; return 0; }
int os_wait_pid_exit(int pid, int ms) { (void)pid; (void)ms; return 1; }

#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    char **v;
    int n, cap;
} OsArgv;

void *os_argv_new(void) {
    OsArgv *a = (OsArgv *)calloc(1, sizeof *a);
    return a;
}

void os_argv_add(void *h, const char *s) {
    OsArgv *a = (OsArgv *)h;
    if (!a) return;
    if (a->n + 2 > a->cap) {
        int cap = a->cap ? a->cap * 2 : 8;
        char **nv = (char **)realloc(a->v, sizeof(char *) * (size_t)cap);
        if (!nv) return;
        a->v = nv;
        a->cap = cap;
    }
    char *c = strdup(s ? s : "");
    if (!c) return;
    a->v[a->n++] = c;
    a->v[a->n] = NULL;
}

void os_argv_free(void *h) {
    OsArgv *a = (OsArgv *)h;
    if (!a) return;
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    free(a);
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "vyto_win.h"

/* Append one argument quoted the way CommandLineToArgvW and the MSVCRT
   startup code split it back: quotes only when needed, a run of backslashes
   doubled only when a quote follows it. */
static int cl_put(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t need = *len + strlen(s) * 2 + 4;
    if (need > *cap) {
        size_t c = *cap ? *cap : 256;
        while (c < need) c *= 2;
        char *nb = (char *)realloc(*buf, c);
        if (!nb) return 0;
        *buf = nb;
        *cap = c;
    }
    char *o = *buf + *len;
    if (*len) *o++ = ' ';
    int quote = (*s == 0) || strpbrk(s, " \t\n\v\"") != NULL;
    if (!quote) {
        size_t n = strlen(s);
        memcpy(o, s, n);
        o += n;
    } else {
        *o++ = '"';
        for (const char *p = s;; p++) {
            size_t bs = 0;
            while (*p == '\\') { bs++; p++; }
            if (*p == 0) {
                /* before the closing quote: every backslash doubled */
                for (size_t i = 0; i < bs * 2; i++) *o++ = '\\';
                break;
            }
            if (*p == '"') {
                for (size_t i = 0; i < bs * 2 + 1; i++) *o++ = '\\';
                *o++ = '"';
            } else {
                for (size_t i = 0; i < bs; i++) *o++ = '\\';
                *o++ = *p;
            }
        }
        *o++ = '"';
    }
    *o = 0;
    *len = (size_t)(o - *buf);
    return 1;
}

/* CreateProcessW for exe + args. The command line is the quoted exe, then the
   quoted arguments; lpApplicationName is the exe, so no PATH search and no
   "C:\Program.exe" ambiguity. */
static int win_start(const char *exe, OsArgv *a, DWORD flags, PROCESS_INFORMATION *pi) {
    char *cl = NULL;
    size_t len = 0, cap = 0;
    if (!cl_put(&cl, &len, &cap, exe)) return 0;
    for (int i = 0; a && i < a->n; i++) {
        if (!cl_put(&cl, &len, &cap, a->v[i])) { free(cl); return 0; }
    }
    VtWBuf we, wc;
    vt_wbuf_from(&we, exe);
    vt_wbuf_from(&wc, cl);
    free(cl);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(pi, 0, sizeof *pi);
    BOOL ok = CreateProcessW(we.p, wc.p, NULL, NULL, FALSE, flags, NULL, NULL, &si, pi);
    vt_wbuf_free(&we);
    vt_wbuf_free(&wc);
    return ok ? 1 : 0;
}

int os_spawn_detached(const char *exe, void *h) {
    PROCESS_INFORMATION pi;
    if (!win_start(exe, (OsArgv *)h, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, &pi)) return -1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)pi.dwProcessId;
}

int os_run_args(const char *exe, void *h, int ms) {
    PROCESS_INFORMATION pi;
    if (!win_start(exe, (OsArgv *)h, 0, &pi)) return -1;
    CloseHandle(pi.hThread);
    DWORD w = WaitForSingleObject(pi.hProcess, ms < 0 ? INFINITE : (DWORD)ms);
    int code;
    if (w == WAIT_OBJECT_0) {
        DWORD c = 0;
        code = GetExitCodeProcess(pi.hProcess, &c) ? (int)c : -1;
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        code = -2;
    }
    CloseHandle(pi.hProcess);
    return code;
}

int os_pid_alive(int pid) {
    if (pid <= 0) return 0;
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED ? 1 : 0;
    int alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}

int os_wait_pid_exit(int pid, int ms) {
    if (pid <= 0) return 1;
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!p) {
        if (GetLastError() != ERROR_ACCESS_DENIED) return 1; /* already gone */
        /* exists but not ours to wait on: poll */
        for (int t = 0; ms < 0 || t < ms; t += 50) {
            if (!os_pid_alive(pid)) return 1;
            Sleep(50);
        }
        return 0;
    }
    DWORD w = WaitForSingleObject(p, ms < 0 ? INFINITE : (DWORD)ms);
    CloseHandle(p);
    return w == WAIT_OBJECT_0;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static char **posix_argv(const char *exe, OsArgv *a) {
    int n = a ? a->n : 0;
    char **v = (char **)malloc(sizeof(char *) * (size_t)(n + 2));
    if (!v) return NULL;
    v[0] = (char *)exe;
    for (int i = 0; i < n; i++) v[i + 1] = a->v[i];
    v[n + 1] = NULL;
    return v;
}

static void nap_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static int read_int(int fd, int *out) {
    char *p = (char *)out;
    size_t got = 0;
    while (got < sizeof *out) {
        ssize_t r = read(fd, p + got, sizeof *out - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

/* Double fork: the middle child starts a new session, forks the real child
   and exits at once, so the real child is re-parented to init (never our
   zombie) and has no controlling terminal. Its stdio is /dev/null: holding
   the caller's pipes would keep whoever reads them waiting for it.

   One close-on-exec pipe carries two answers: the middle child writes the
   grandchild's pid; the grandchild writes errno only if exec fails. EOF
   after the pid means exec succeeded. */
int os_spawn_detached(const char *exe, void *h) {
    char **argv = posix_argv(exe, (OsArgv *)h);
    if (!argv) return -1;
    int fd[2];
    if (pipe(fd) != 0) { free(argv); return -1; }
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    pid_t mid = fork();
    if (mid < 0) { close(fd[0]); close(fd[1]); free(argv); return -1; }
    if (mid == 0) {
        close(fd[0]);
        setsid();
        pid_t kid = fork();
        if (kid < 0) _exit(127);
        if (kid > 0) {
            int k = (int)kid;
            ssize_t w = write(fd[1], &k, sizeof k);
            (void)w;
            _exit(0);
        }
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        signal(SIGPIPE, SIG_DFL);
        execv(exe, argv);
        int e = errno;
        ssize_t w = write(fd[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(fd[1]);
    free(argv);
    int st;
    while (waitpid(mid, &st, 0) < 0 && errno == EINTR) {}
    int kid = -1, err;
    if (!read_int(fd[0], &kid)) kid = -1;
    else if (read_int(fd[0], &err)) kid = -1; /* exec failed */
    close(fd[0]);
    return kid;
}

int os_run_args(const char *exe, void *h, int ms) {
    char **argv = posix_argv(exe, (OsArgv *)h);
    if (!argv) return -1;
    int fd[2];
    if (pipe(fd) != 0) { free(argv); return -1; }
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    pid_t kid = fork();
    if (kid < 0) { close(fd[0]); close(fd[1]); free(argv); return -1; }
    if (kid == 0) {
        close(fd[0]);
        signal(SIGPIPE, SIG_DFL);
        execv(exe, argv);
        int e = errno;
        ssize_t w = write(fd[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(fd[1]);
    free(argv);
    int err;
    int failed = read_int(fd[0], &err);
    close(fd[0]);
    int st = 0;
    if (failed) {
        while (waitpid(kid, &st, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    for (int t = 0;; t += 10) {
        pid_t r = waitpid(kid, &st, ms < 0 ? 0 : WNOHANG);
        if (r == kid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (ms >= 0 && t >= ms) {
            kill(kid, SIGKILL);
            while (waitpid(kid, &st, 0) < 0 && errno == EINTR) {}
            return -2;
        }
        if (ms >= 0) nap_ms(10);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int os_pid_alive(int pid) {
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) {
        /* our own exited-but-unreaped child still answers kill(0) */
        int st;
        if (waitpid((pid_t)pid, &st, WNOHANG) == (pid_t)pid) return 0;
        return 1;
    }
    return errno == EPERM;
}

int os_wait_pid_exit(int pid, int ms) {
    for (int t = 0;; t += 20) {
        if (!os_pid_alive(pid)) return 1;
        if (ms >= 0 && t >= ms) return 0;
        nap_ms(20);
    }
}

#endif /* _WIN32 */
#endif /* VT_NO_LIBC */
