/* vyto/desktop/service — run a Vyto program as a Windows service or a systemd unit,
 * and install/remove one. See ../../README.md.
 *
 * The Vyto code never runs on a foreign thread. On Windows the Service
 * Control Manager wants StartServiceCtrlDispatcherW to block a thread for the
 * life of the service and calls ServiceMain and the control handler on its
 * own; here that is a helper thread we start, and it talks to the Vyto main
 * thread only through events: `ready` (ServiceMain is up, or there is no SCM),
 * `stop` (the SCM, or Ctrl+C in a console, asked us to stop) and `done` (the
 * Vyto body returned, report STOPPED). The runtime is single-threaded, so no
 * callback into Vyto is ever made from C.
 *
 * On POSIX a stop is SIGTERM (what systemd sends) or SIGINT; the handler sets
 * a flag and writes to a self-pipe so vsvc_wait wakes at once. Under systemd
 * (NOTIFY_SOCKET set) READY=1 and STOPPING=1 are sent, so a unit can use
 * Type=notify. Install/uninstall write the unit file and drive systemctl,
 * started from an argv array, never a shell.
 *
 * Freestanding and Android: stub arm, every call reports failure.
 *
 * Every Windows function used is in the audited Windows 7 export list. */
#include <stdint.h>

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vyto_win.h"

static wchar_t g_name[257];
static HANDLE g_ready, g_stop, g_done, g_thread;
static SERVICE_STATUS_HANDLE g_handle;
static SERVICE_STATUS g_status;
static volatile LONG g_under_scm;
static DWORD g_exit_code;
static char g_err[512];

static void set_err(const char *what, DWORD e) {
    char sys[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0,
                   sys, sizeof sys, NULL);
    size_t n = strlen(sys);
    while (n > 0 && (sys[n - 1] == '\n' || sys[n - 1] == '\r' || sys[n - 1] == '.')) sys[--n] = 0;
    snprintf(g_err, sizeof g_err, "%s: %s (error %lu)", what, sys, (unsigned long)e);
}

static void report(DWORD state, DWORD wait_hint) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = state == SERVICE_STOPPED && g_exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    g_status.dwServiceSpecificExitCode = g_exit_code;
    g_status.dwWaitHint = wait_hint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    if (g_handle) SetServiceStatus(g_handle, &g_status);
}

static DWORD WINAPI handler(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx) {
    (void)type; (void)data; (void)ctx;
    switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        /* 30 s for the Vyto body to notice stopping() and return */
        report(SERVICE_STOP_PENDING, 30000);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
    (void)argc; (void)argv;
    g_handle = RegisterServiceCtrlHandlerExW(g_name, handler, NULL);
    if (!g_handle) { SetEvent(g_ready); return; }
    report(SERVICE_START_PENDING, 5000);
    InterlockedExchange(&g_under_scm, 1);
    report(SERVICE_RUNNING, 0);
    SetEvent(g_ready);
    /* Hold ServiceMain open until the Vyto body has returned. */
    WaitForSingleObject(g_done, INFINITE);
    report(SERVICE_STOPPED, 0);
}

static DWORD WINAPI dispatcher(LPVOID p) {
    (void)p;
    SERVICE_TABLE_ENTRYW table[2];
    table[0].lpServiceName = g_name;
    table[0].lpServiceProc = service_main;
    table[1].lpServiceName = NULL;
    table[1].lpServiceProc = NULL;
    /* Fails at once with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT when the
       process was not started by the SCM: that is console mode. */
    if (!StartServiceCtrlDispatcherW(table)) SetEvent(g_ready);
    return 0;
}

static BOOL WINAPI console_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_SHUTDOWN_EVENT || type == CTRL_LOGOFF_EVENT) {
        SetEvent(g_stop);
        return TRUE;
    }
    return FALSE;
}

/* 1 = started by the Service Control Manager, 0 = console, -1 = error. */
int32_t vsvc_begin(const char *name) {
    VtWBuf w;
    vt_wbuf_from(&w, name ? name : "");
    wcsncpy(g_name, w.p, 256);
    g_name[256] = 0;
    vt_wbuf_free(&w);
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ready || !g_stop || !g_done) { set_err("CreateEvent", GetLastError()); return -1; }
    g_thread = CreateThread(NULL, 0, dispatcher, NULL, 0, NULL);
    if (!g_thread) { set_err("CreateThread", GetLastError()); return -1; }
    WaitForSingleObject(g_ready, INFINITE);
    if (g_under_scm) return 1;
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    return 0;
}

int32_t vsvc_stopping(void) {
    return g_stop && WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0;
}

int32_t vsvc_wait(int32_t ms) {
    if (!g_stop) return 0;
    return WaitForSingleObject(g_stop, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}

void vsvc_end(int32_t code) {
    g_exit_code = (DWORD)code;
    if (g_under_scm && g_done) {
        SetEvent(g_done);
        if (g_thread) WaitForSingleObject(g_thread, 10000);
    }
}

/* Services have no console; their log goes to
   %ProgramData%\<name>\<name>.log. In a console it goes to stderr. */
int32_t vsvc_log(const char *msg) {
    if (!g_under_scm) { fprintf(stderr, "%s\n", msg); fflush(stderr); return 1; }
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &base)) || !base) return 0;
    wchar_t dir[1024], file[1100];
    _snwprintf(dir, 1023, L"%ls\\%ls", base, g_name);
    dir[1023] = 0;
    CoTaskMemFree(base);
    CreateDirectoryW(dir, NULL);
    _snwprintf(file, 1099, L"%ls\\%ls.log", dir, g_name);
    file[1099] = 0;
    FILE *f = _wfopen(file, L"ab");
    if (!f) return 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, msg);
    fclose(f);
    return 1;
}

const char *vsvc_error(void) { return g_err; }

/* Which install mechanism this platform uses: 1 the Service Control Manager,
   0 systemd unit files. */
int32_t vsvc_uses_scm(void) { return 1; }

/* The running exe's own path, UTF-8 (for installing "this program"). */
const char *vsvc_self_path(void) {
    static char out[4096];
    wchar_t p[32768];
    DWORD n = GetModuleFileNameW(NULL, p, 32768);
    if (n == 0 || n >= 32768) return "";
    char *u = vt_utf8_from_wn(p, (int)n);
    if (!u) return "";
    snprintf(out, sizeof out, "%s", u);
    free(u);
    return out;
}

/* ---- install / control (needs administrator rights) ---- */

static SC_HANDLE open_scm(DWORD access) {
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, access);
    if (!m) set_err("OpenSCManager", GetLastError());
    return m;
}

/* start: 0 manual, 1 automatic, 2 automatic (delayed). The binary path is
   quoted, so a path with spaces ("C:\Program Files\...") cannot be
   misread as a different program — the classic unquoted-service-path
   hole. */
int32_t vsvc_install(const char *name, const char *display, const char *desc,
                     const char *exe, const char *args, int32_t start) {
    SC_HANDLE m = open_scm(SC_MANAGER_CREATE_SERVICE);
    if (!m) return 0;
    char cmd[8192];
    if (args && *args) snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    else snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    VtWBuf wn, wd, wc;
    vt_wbuf_from(&wn, name);
    vt_wbuf_from(&wd, display && *display ? display : name);
    vt_wbuf_from(&wc, cmd);
    SC_HANDLE s = CreateServiceW(m, wn.p, wd.p, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 start == 0 ? SERVICE_DEMAND_START : SERVICE_AUTO_START,
                                 SERVICE_ERROR_NORMAL, wc.p, NULL, NULL, NULL, NULL, NULL);
    int ok = 0;
    if (!s) {
        set_err("CreateService", GetLastError());
    } else {
        if (desc && *desc) {
            VtWBuf wx;
            vt_wbuf_from(&wx, desc);
            SERVICE_DESCRIPTIONW sd;
            sd.lpDescription = wx.p;
            ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &sd);
            vt_wbuf_free(&wx);
        }
        if (start == 2) {
            SERVICE_DELAYED_AUTO_START_INFO di;
            di.fDelayedAutostart = TRUE;
            ChangeServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &di);
        }
        /* restart after a crash: 5 s, 30 s, then every 60 s */
        SC_ACTION acts[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 },
                              { SC_ACTION_RESTART, 60000 } };
        SERVICE_FAILURE_ACTIONSW fa;
        memset(&fa, 0, sizeof fa);
        fa.dwResetPeriod = 86400;
        fa.cActions = 3;
        fa.lpsaActions = acts;
        ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        CloseServiceHandle(s);
        ok = 1;
    }
    vt_wbuf_free(&wn);
    vt_wbuf_free(&wd);
    vt_wbuf_free(&wc);
    CloseServiceHandle(m);
    return ok;
}

static SC_HANDLE open_svc(const char *name, DWORD access, SC_HANDLE *m) {
    *m = open_scm(SC_MANAGER_CONNECT);
    if (!*m) return NULL;
    VtWBuf wn;
    vt_wbuf_from(&wn, name);
    SC_HANDLE s = OpenServiceW(*m, wn.p, access);
    vt_wbuf_free(&wn);
    if (!s) set_err("OpenService", GetLastError());
    return s;
}

/* -1 not installed, else a SERVICE_* state (1 stopped .. 4 running). */
int32_t vsvc_state(const char *name) {
    SC_HANDLE m;
    SC_HANDLE s = open_svc(name, SERVICE_QUERY_STATUS, &m);
    if (!s) { if (m) CloseServiceHandle(m); return -1; }
    SERVICE_STATUS_PROCESS sp;
    DWORD need = 0;
    int32_t st = -1;
    if (QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (LPBYTE)&sp, sizeof sp, &need))
        st = (int32_t)sp.dwCurrentState;
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    return st;
}

static int wait_state(SC_HANDLE s, DWORD want, DWORD ms) {
    DWORD waited = 0;
    for (;;) {
        SERVICE_STATUS_PROCESS sp;
        DWORD need = 0;
        if (!QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (LPBYTE)&sp, sizeof sp, &need)) return 0;
        if (sp.dwCurrentState == want) return 1;
        if (waited >= ms) return 0;
        Sleep(250);
        waited += 250;
    }
}

int32_t vsvc_start(const char *name) {
    SC_HANDLE m;
    SC_HANDLE s = open_svc(name, SERVICE_START | SERVICE_QUERY_STATUS, &m);
    if (!s) { if (m) CloseServiceHandle(m); return 0; }
    int ok = StartServiceW(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    if (!ok) set_err("StartService", GetLastError());
    else ok = wait_state(s, SERVICE_RUNNING, 15000);
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    return ok;
}

int32_t vsvc_stop(const char *name, int32_t timeout_ms) {
    SC_HANDLE m;
    SC_HANDLE s = open_svc(name, SERVICE_STOP | SERVICE_QUERY_STATUS, &m);
    if (!s) { if (m) CloseServiceHandle(m); return 0; }
    SERVICE_STATUS st;
    int ok = ControlService(s, SERVICE_CONTROL_STOP, &st) ||
             GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    if (!ok) set_err("ControlService", GetLastError());
    else ok = wait_state(s, SERVICE_STOPPED, timeout_ms > 0 ? (DWORD)timeout_ms : 30000);
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    return ok;
}

int32_t vsvc_uninstall(const char *name) {
    vsvc_stop(name, 30000);
    SC_HANDLE m;
    SC_HANDLE s = open_svc(name, DELETE, &m);
    if (!s) { if (m) CloseServiceHandle(m); return 0; }
    int ok = DeleteService(s) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    if (!ok) set_err("DeleteService", GetLastError());
    CloseServiceHandle(s);
    CloseServiceHandle(m);
    return ok;
}

/* ======================================================================== */
#elif !defined(VT_NO_LIBC) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0      /* macOS: no such flag; SIGPIPE cannot fire on a datagram anyway */
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

static volatile sig_atomic_t g_stop;
static int g_pipe[2] = { -1, -1 };
static int g_mode;
static char g_err[512];

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_pipe[1] >= 0) {
        int saved = errno;
        ssize_t r = write(g_pipe[1], "x", 1);
        (void)r;
        errno = saved;
    }
}

/* systemd's readiness protocol, without libsystemd: one datagram to
   $NOTIFY_SOCKET ('@' prefix = abstract namespace). */
static void sd_notify_msg(const char *msg) {
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof a.sun_path) return;
    memcpy(a.sun_path, path, n);
    if (a.sun_path[0] == '@') a.sun_path[0] = 0;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&a,
           (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n));
    close(fd);
}

int32_t vsvc_begin(const char *name) {
    (void)name;
    if (pipe(g_pipe) != 0) { snprintf(g_err, sizeof g_err, "pipe: %s", strerror(errno)); return -1; }
    for (int i = 0; i < 2; i++) {
        fcntl(g_pipe[i], F_SETFL, fcntl(g_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(g_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    /* Started by systemd? NOTIFY_SOCKET says so for Type=notify units.
       INVOCATION_ID alone does not: it is inherited by everything a systemd
       unit spawns, a whole desktop session included, so a program run from a
       terminal has it too. systemd gives a service /dev/null for stdin, a
       terminal gives a tty, which tells the two apart. */
    const char *ns = getenv("NOTIFY_SOCKET");
    const char *inv = getenv("INVOCATION_ID");
    g_mode = (ns && *ns) || (inv && *inv && !isatty(0)) ? 1 : 0;
    sd_notify_msg("READY=1");
    return g_mode;
}

int32_t vsvc_stopping(void) { return g_stop != 0; }

int32_t vsvc_wait(int32_t ms) {
    if (g_stop) return 1;
    if (g_pipe[0] < 0) return 0;
    struct pollfd p;
    p.fd = g_pipe[0];
    p.events = POLLIN;
    int r = poll(&p, 1, ms < 0 ? -1 : ms);
    if (r > 0) {
        char buf[64];
        while (read(g_pipe[0], buf, sizeof buf) > 0) {}
    }
    return g_stop != 0;
}

void vsvc_end(int32_t code) {
    (void)code;
    sd_notify_msg("STOPPING=1");
}

/* journald captures stderr, with the unit name attached. */
int32_t vsvc_log(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
    return 1;
}

const char *vsvc_error(void) { return g_err; }

int32_t vsvc_uses_scm(void) { return 0; }

const char *vsvc_self_path(void) {
    static char out[4096];
    ssize_t n = readlink("/proc/self/exe", out, sizeof out - 1);
    if (n <= 0) return "";
    out[n] = 0;
    return out;
}

/* systemctl from an argv array; returns its exit status (-1: not started) */
static int run_systemctl(const char *a1, const char *a2) {
    char *argv[5];
    int i = 0;
    argv[i++] = (char *)"systemctl";
    argv[i++] = (char *)a1;
    if (a2) argv[i++] = (char *)a2;
    argv[i] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, "systemctl", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { snprintf(g_err, sizeof g_err, "cannot run systemctl"); return -1; }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* unit_dir: where unit files go — "/etc/systemd/system" normally; the
   argument exists so tests (and packagers) can write elsewhere. */
static const char *unit_dir(void) {
    const char *d = getenv("VYTO_SYSTEMD_DIR");
    return d && *d ? d : "/etc/systemd/system";
}

/* `unit` is the full unit text (desktop.vt's systemdUnit builds it). */
int32_t vsvc_install_unit(const char *name, const char *unit, int32_t enable) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.service", unit_dir(), name);
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(g_err, sizeof g_err, "cannot write %s: %s", path, strerror(errno)); return 0; }
    fputs(unit, f);
    fclose(f);
    if (getenv("VYTO_SYSTEMD_DIR")) return 1;   /* a test dir: no systemctl */
    if (run_systemctl("daemon-reload", NULL) != 0) return 0;
    if (enable && run_systemctl("enable", name) != 0) {
        snprintf(g_err, sizeof g_err, "systemctl enable %s failed", name);
        return 0;
    }
    return 1;
}

/* The Windows-shaped install entry point has no meaning here; the Vyto side
   calls vsvc_install_unit on POSIX instead. */
int32_t vsvc_install(const char *name, const char *display, const char *desc,
                     const char *exe, const char *args, int32_t start) {
    (void)name; (void)display; (void)desc; (void)exe; (void)args; (void)start;
    snprintf(g_err, sizeof g_err, "use the unit-file install on this platform");
    return 0;
}

int32_t vsvc_uninstall(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.service", unit_dir(), name);
    if (!getenv("VYTO_SYSTEMD_DIR")) {
        run_systemctl("stop", name);
        run_systemctl("disable", name);
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        snprintf(g_err, sizeof g_err, "cannot remove %s: %s", path, strerror(errno));
        return 0;
    }
    if (!getenv("VYTO_SYSTEMD_DIR")) run_systemctl("daemon-reload", NULL);
    return 1;
}

int32_t vsvc_start(const char *name) { return run_systemctl("start", name) == 0; }
int32_t vsvc_stop(const char *name, int32_t timeout_ms) {
    (void)timeout_ms;
    return run_systemctl("stop", name) == 0;
}

/* -1 not installed, 1 stopped, 4 running (the Windows numbering). */
int32_t vsvc_state(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.service", unit_dir(), name);
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (getenv("VYTO_SYSTEMD_DIR")) return 1;
    return run_systemctl("is-active", name) == 0 ? 4 : 1;
}

/* ======================================================================== */
#else /* freestanding or Android */

int32_t vsvc_begin(const char *name) { (void)name; return -1; }
int32_t vsvc_stopping(void) { return 0; }
int32_t vsvc_wait(int32_t ms) { (void)ms; return 0; }
void vsvc_end(int32_t code) { (void)code; }
int32_t vsvc_log(const char *msg) { (void)msg; return 0; }
const char *vsvc_error(void) { return "services are not available on this target"; }
int32_t vsvc_uses_scm(void) { return 0; }
const char *vsvc_self_path(void) { return ""; }
int32_t vsvc_install(const char *name, const char *display, const char *desc,
                     const char *exe, const char *args, int32_t start) {
    (void)name; (void)display; (void)desc; (void)exe; (void)args; (void)start;
    return 0;
}
int32_t vsvc_install_unit(const char *name, const char *unit, int32_t enable) {
    (void)name; (void)unit; (void)enable;
    return 0;
}
int32_t vsvc_uninstall(const char *name) { (void)name; return 0; }
int32_t vsvc_start(const char *name) { (void)name; return 0; }
int32_t vsvc_stop(const char *name, int32_t timeout_ms) { (void)name; (void)timeout_ms; return 0; }
int32_t vsvc_state(const char *name) { (void)name; return -1; }

#endif

#if defined(_WIN32) && !defined(VT_NO_LIBC)
/* Unit files are a POSIX concept; keep the symbol so one extern block serves
   every target. */
int32_t vsvc_install_unit(const char *name, const char *unit, int32_t enable) {
    (void)name; (void)unit; (void)enable;
    snprintf(g_err, sizeof g_err, "systemd units are not used on Windows");
    return 0;
}
#endif
