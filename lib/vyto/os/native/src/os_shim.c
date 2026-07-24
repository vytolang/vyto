/* vyto/os native backing — environment, working directory, process info, and
   subprocess execution.

   Fixed-size queries (cwd, hostname, arch) fill a caller `bytes(cap)` buffer
   and NUL-terminate. `os_capture` uses the opaque-handle + explicit-free shape
   (mirroring net_shim.c): the buffer it mallocs is owned by a Vyto ProcResult
   whose deinit calls os_capture_free exactly once. */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
/* Win32 has none of the POSIX process/user headers. Everything below is served
   by kernel32 (auto-linked) or the CRT, so vyto/os needs no #link entries. */
#include <windows.h>
#include <direct.h>
#include <process.h>
#define popen  _popen
#define pclose _pclose
#else
#include <unistd.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#endif

const char *os_getenv(const char *k) { return getenv(k); }

/* Resolved stdlib directory, for locating vendored assets (e.g. fonts) at
   runtime. Runtime VYTO_HOME wins; otherwise the compiler bakes the path it
   resolved via -DVYTO_LIBDIR. Empty string when neither is available. */
const char *os_lib_dir(void) {
    static char buf[4096];
    static int cached = 0;
    if (cached) return buf;
    cached = 1;
    const char *e = getenv("VYTO_HOME");
    if (e && *e) { snprintf(buf, sizeof buf, "%s/lib", e); return buf; }
#ifdef VYTO_LIBDIR
    snprintf(buf, sizeof buf, "%s", VYTO_LIBDIR);
#else
    buf[0] = 0;
#endif
    return buf;
}

/* The app's own directory (entry file's dir), for locating its assets/conf/
   storage at runtime. Runtime VYTO_APP_DIR wins; otherwise the compiler bakes
   the path via -DVYTO_APPDIR. Empty string when neither is available. */
const char *os_app_dir(void) {
    static char buf[4096];
    static int cached = 0;
    if (cached) return buf;
    cached = 1;
    const char *e = getenv("VYTO_APP_DIR");
    if (e && *e) { snprintf(buf, sizeof buf, "%s", e); return buf; }
#ifdef VYTO_APPDIR
    snprintf(buf, sizeof buf, "%s", VYTO_APPDIR);
#else
    buf[0] = 0;
#endif
    return buf;
}
#ifdef _WIN32
/* _putenv_s with an empty value both sets-to-empty and removes on Windows —
   the CRT drops the variable entirely, which is what unsetenv means. */
int os_setenv(const char *k, const char *v) { return _putenv_s(k, v) == 0 ? 0 : -1; }
int os_unsetenv(const char *k) { return _putenv_s(k, "") == 0 ? 0 : -1; }
int os_chdir(const char *p) { return _chdir(p); }

int os_getcwd(char *buf, int cap) {
    if (!_getcwd(buf, cap)) { if (cap > 0) buf[0] = 0; return 0; }
    return (int)strlen(buf);
}
int os_pid(void) { return (int)_getpid(); }

/* GetComputerNameA rather than gethostname: the latter would drag in winsock
   (-lws2_32) and require WSAStartup for one string. */
int os_gethostname(char *buf, int cap) {
    DWORD n = (DWORD)cap;
    if (cap <= 0) return 0;
    if (!GetComputerNameA(buf, &n)) { buf[0] = 0; return 0; }
    buf[cap - 1] = 0;
    return (int)strlen(buf);
}

/* The environment, not GetUserNameA/SHGetFolderPath — keeps advapi32/shell32
   off the link line. Both variables are set for every interactive session. */
const char *os_username(void) {
    const char *u = getenv("USERNAME");
    return u ? u : "";
}
const char *os_homedir(void) {
    const char *h = getenv("USERPROFILE");
    if (h && *h) return h;
    /* pre-USERPROFILE fallback: HOMEDRIVE + HOMEPATH, joined once into a static */
    static char buf[4096];
    const char *d = getenv("HOMEDRIVE"), *p = getenv("HOMEPATH");
    if (d && p) { snprintf(buf, sizeof buf, "%s%s", d, p); return buf; }
    return "";
}
int os_cpucount(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}
#else
int os_setenv(const char *k, const char *v) { return setenv(k, v, 1); }
int os_unsetenv(const char *k) { return unsetenv(k); }
int os_chdir(const char *p) { return chdir(p); }

int os_getcwd(char *buf, int cap) {
    if (!getcwd(buf, (size_t)cap)) { if (cap > 0) buf[0] = 0; return 0; }
    return (int)strlen(buf);
}
int os_pid(void) { return (int)getpid(); }

int os_gethostname(char *buf, int cap) {
    if (gethostname(buf, (size_t)cap) != 0) { if (cap > 0) buf[0] = 0; return 0; }
    buf[cap - 1] = 0;
    return (int)strlen(buf);
}

const char *os_username(void) {
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : "";
}
const char *os_homedir(void) {
    const char *h = getenv("HOME");
    if (h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "";
}
int os_cpucount(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif
void os_exit(int code) { exit(code); }

const char *os_platform(void) {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}
int os_arch(char *buf, int cap) {
    if (cap <= 0) return 0;
#ifdef _WIN32
    /* GetNativeSystemInfo, not GetSystemInfo: under WOW64 the latter reports the
       emulated x86 architecture. Names are normalized to the uname(2) spellings
       so `arch()` reads the same on every platform. */
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *m;
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: m = "x86_64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: m = "aarch64"; break;
    case PROCESSOR_ARCHITECTURE_ARM:   m = "arm"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: m = "i686"; break;
    default:                           m = "unknown"; break;
    }
    strncpy(buf, m, (size_t)cap - 1);
#else
    struct utsname u;
    if (uname(&u) != 0) { buf[0] = 0; return 0; }
    strncpy(buf, u.machine, (size_t)cap - 1);
#endif
    buf[cap - 1] = 0;
    return (int)strlen(buf);
}

/* Run through the shell; return the child's exit status (or -1 if it could not
   be launched / did not exit normally). */
int os_run(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
#ifdef _WIN32
    /* The MSVCRT system() returns the child's exit code directly — there is no
       wait(2) status word to unpack, and no WIFEXITED to unpack it with. */
    return st;
#else
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

typedef struct {
    char *data;
    long len;
    int code;
} OsCap;

OsCap *os_capture(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    OsCap *c = (OsCap *)calloc(1, sizeof *c);
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!c || !buf) { free(c); free(buf); pclose(p); return NULL; }
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, p)) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); free(c); pclose(p); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = 0;
    int st = pclose(p);
    c->data = buf;
    c->len = (long)len;
#ifdef _WIN32
    c->code = st; /* _pclose returns the exit code directly — see os_run */
#else
    c->code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
    return c;
}
const char *os_capture_data(OsCap *c) { return c ? c->data : ""; }
long os_capture_len(OsCap *c) { return c ? c->len : 0; }
int os_capture_code(OsCap *c) { return c ? c->code : -1; }
void os_capture_free(OsCap *c) {
    if (!c) return;
    free(c->data);
    free(c);
}
