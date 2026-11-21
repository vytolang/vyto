/* vyto/os native backing — environment, working directory, process info, and
   subprocess execution.

   Fixed-size queries (cwd, hostname, arch) fill a caller `bytes(cap)` buffer
   and NUL-terminate. `os_capture` uses the opaque-handle + explicit-free shape
   (mirroring net_shim.c): the buffer it mallocs is owned by a Vyto ProcResult
   whose deinit calls os_capture_free exactly once. */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
/* Win32 has none of the POSIX process/user headers. Everything below is served
   by kernel32 (auto-linked) or the CRT, so vyto/os needs no #link entries. */
#include <windows.h>
#include <direct.h>
#include <process.h>
#include "vyto_win.h" /* UTF-8 <-> UTF-16 at every CRT/Win32 boundary */
#else
#include <unistd.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#endif

#ifdef _WIN32
/* The narrow getenv() returns values in the ANSI code page, so a profile path
   like C:\Users\Zoë comes back mangled. Read the wide environment and hand
   back UTF-8. The result has to outlive this call without an owner, so it
   lives in a small ring of slots: Vyto copies it at once (str()), and the C
   callers here use one or two values at a time. NULL when unset, like
   getenv. */
static const char *os_wgetenv(const char *k) {
    static char *ring[8];
    static int next;
    VtWBuf wk;
    if (!k || !vt_wbuf_from(&wk, k)) return NULL;
    const wchar_t *v = _wgetenv(wk.p);
    vt_wbuf_free(&wk);
    if (!v) return NULL;
    char *u = vt_utf8_from_w(v);
    if (!u) return NULL;
    free(ring[next]);
    ring[next] = u;
    next = (next + 1) % 8;
    return u;
}
#define getenv(k) os_wgetenv(k)
#endif

const char *os_getenv(const char *k) { return getenv(k); }

#ifdef _WIN32
/* The directory holding the running exe, as UTF-8 with forward slashes, so
   appDir() + "/assets/x.png" reads the same on every platform. 0 on failure.
   This replaces the compile-time paths below on Windows: those name a
   directory on the (Linux) build host, which does not exist where the exe
   runs. */
static int exe_dir(char *out, size_t cap) {
    static WCHAR w[32768];  /* static: 64 KB is too much stack for a helper */
    DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    if (n == 0 || n >= sizeof w / sizeof w[0]) return 0;
    while (n > 0 && w[n - 1] != L'\\' && w[n - 1] != L'/') n--;
    if (n > 1) n--;           /* drop the separator, keep "C:\" whole below */
    if (n == 2 && w[1] == L':') n = 3;
    char *u = vt_utf8_from_wn(w, (int)n);
    if (!u) return 0;
    size_t len = strlen(u);
    if (len + 1 > cap) { free(u); return 0; }
    memcpy(out, u, len + 1);
    free(u);
    for (char *c = out; *c; c++)
        if (*c == '\\') *c = '/';
    return 1;
}

static int dir_exists(const char *utf8) {
    VtWBuf w;
    if (!vt_wbuf_from(&w, utf8)) { vt_wbuf_free(&w); return 0; }
    DWORD a = GetFileAttributesW(w.p);
    vt_wbuf_free(&w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
#endif

/* Resolved stdlib directory, for locating vendored assets (e.g. fonts) at
   runtime. Runtime VYTO_HOME wins. On Windows it is <exe dir>/lib, else
   <exe dir>/../lib, the same layouts vytoc itself searches; elsewhere the
   compiler bakes the path it resolved via -DVYTO_LIBDIR. Empty string when
   none is available. */
const char *os_lib_dir(void) {
    static char buf[4096];
    static int cached = 0;
    if (cached) return buf;
    cached = 1;
    const char *e = getenv("VYTO_HOME");
    if (e && *e) { snprintf(buf, sizeof buf, "%s/lib", e); return buf; }
#ifdef _WIN32
    char dir[4096];
    buf[0] = 0;
    if (exe_dir(dir, sizeof dir)) {
        snprintf(buf, sizeof buf, "%s/lib", dir);
        if (!dir_exists(buf)) snprintf(buf, sizeof buf, "%s/../lib", dir);
        if (!dir_exists(buf)) buf[0] = 0;
    }
#elif defined(VYTO_LIBDIR)
    snprintf(buf, sizeof buf, "%s", VYTO_LIBDIR);
#else
    buf[0] = 0;
#endif
    return buf;
}

/* The app's own directory, for locating its assets/conf/storage at runtime.
   Runtime VYTO_APP_DIR wins. On Windows it is the directory holding the exe,
   because an exe is shipped with its assets beside it; elsewhere the compiler
   bakes the entry file's directory via -DVYTO_APPDIR. Empty string when none
   is available. */
const char *os_app_dir(void) {
    static char buf[4096];
    static int cached = 0;
    if (cached) return buf;
    cached = 1;
    const char *e = getenv("VYTO_APP_DIR");
    if (e && *e) { snprintf(buf, sizeof buf, "%s", e); return buf; }
#ifdef _WIN32
    if (!exe_dir(buf, sizeof buf)) buf[0] = 0;
#elif defined(VYTO_APPDIR)
    snprintf(buf, sizeof buf, "%s", VYTO_APPDIR);
#else
    buf[0] = 0;
#endif
    return buf;
}
#ifdef _WIN32
/* _wputenv_s with an empty value both sets-to-empty and removes on Windows —
   the CRT drops the variable entirely, which is what unsetenv means.
   The wide calls keep msvcrt's narrow and wide environments in step, and
   pass the change on to child processes. */
int os_setenv(const char *k, const char *v) {
    VtWBuf wk, wv;
    vt_wbuf_from(&wk, k);
    vt_wbuf_from(&wv, v);
    int r = _wputenv_s(wk.p, wv.p) == 0 ? 0 : -1;
    vt_wbuf_free(&wk);
    vt_wbuf_free(&wv);
    return r;
}
int os_unsetenv(const char *k) { return os_setenv(k, ""); }
int os_chdir(const char *p) {
    VtWBuf w;
    vt_wbuf_from(&w, p);
    int r = _wchdir(w.p);
    vt_wbuf_free(&w);
    return r;
}

int os_getcwd(char *buf, int cap) {
    if (cap <= 0) return 0;
    buf[0] = 0;
    wchar_t *w = _wgetcwd(NULL, 0);  /* malloc'd, any length */
    if (!w) return 0;
    char *u = vt_utf8_from_w(w);
    free(w);
    if (!u) return 0;
    size_t n = strlen(u);
    if ((int)n >= cap) { free(u); return 0; }  /* too small: report failure, not a cut path */
    memcpy(buf, u, n + 1);
    free(u);
    return (int)n;
}
int os_pid(void) { return (int)_getpid(); }

/* GetComputerNameW rather than gethostname: the latter would drag in winsock
   (-lws2_32) and require WSAStartup for one string. */
int os_gethostname(char *buf, int cap) {
    WCHAR w[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (cap <= 0) return 0;
    buf[0] = 0;
    if (!GetComputerNameW(w, &n)) return 0;
    char *u = vt_utf8_from_wn(w, (int)n);
    if (!u) return 0;
    strncpy(buf, u, (size_t)cap - 1);
    buf[cap - 1] = 0;
    free(u);
    return (int)strlen(buf);
}

/* The OS install's identity: HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid,
   written when Windows is installed (present on Win7). KEY_WOW64_64KEY reads
   the 64-bit view even from a 32-bit process. advapi32 is looked up at run
   time rather than #link-ed, so vyto/os keeps needing no link entries; every
   Win32 process already has it loaded. 0 (and "") on any failure. */
typedef LONG (WINAPI *os_RegOpenKeyExW)(HKEY, LPCWSTR, DWORD, REGSAM, HKEY *);
typedef LONG (WINAPI *os_RegQueryValueExW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *os_RegCloseKey)(HKEY);
int os_machine_id(char *buf, int cap) {
    if (cap <= 0) return 0;
    buf[0] = 0;
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return 0;
    os_RegOpenKeyExW open = (os_RegOpenKeyExW)(void *)GetProcAddress(adv, "RegOpenKeyExW");
    os_RegQueryValueExW query = (os_RegQueryValueExW)(void *)GetProcAddress(adv, "RegQueryValueExW");
    os_RegCloseKey close = (os_RegCloseKey)(void *)GetProcAddress(adv, "RegCloseKey");
    int n = 0;
    HKEY k;
    if (open && query && close &&
        open(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
             KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        WCHAR w[128];
        DWORD type = 0, sz = sizeof w - sizeof(WCHAR);
        if (query(k, L"MachineGuid", NULL, &type, (LPBYTE)w, &sz) == ERROR_SUCCESS &&
            type == REG_SZ && sz >= sizeof(WCHAR)) {
            w[sz / sizeof(WCHAR)] = 0;
            char *u = vt_utf8_from_w(w);
            if (u) {
                strncpy(buf, u, (size_t)cap - 1);
                buf[cap - 1] = 0;
                free(u);
                n = (int)strlen(buf);
            }
        }
        close(k);
    }
    FreeLibrary(adv);
    return n;
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

/* The OS install's identity: systemd's /etc/machine-id, else the older D-Bus
   copy. First line only, trailing whitespace dropped. 0 (and "") when neither
   exists — macOS, BSDs, minimal containers. */
int os_machine_id(char *buf, int cap) {
    static const char *paths[] = { "/etc/machine-id", "/var/lib/dbus/machine-id" };
    if (cap <= 0) return 0;
    buf[0] = 0;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        if (!fgets(buf, cap, f)) buf[0] = 0;
        fclose(f);
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                         buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
        if (n > 0) return (int)n;
    }
    return 0;
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
#ifdef _WIN32
    VtWBuf w;
    vt_wbuf_from(&w, cmd);
    int st = _wsystem(w.p);
    vt_wbuf_free(&w);
#else
    int st = system(cmd);
#endif
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

#ifdef _WIN32
/* The command line goes in as UTF-16, so non-ASCII arguments reach the child.
   Its output comes back as the bytes it wrote: console programs on Windows
   write in the OEM code page unless they choose otherwise. */
static FILE *os_popen(const char *cmd) {
    VtWBuf w;
    vt_wbuf_from(&w, cmd);
    FILE *p = _wpopen(w.p, L"r");
    vt_wbuf_free(&w);
    return p;
}
#define pclose _pclose
#else
#define os_popen(cmd) popen((cmd), "r")
#endif

OsCap *os_capture(const char *cmd) {
    FILE *p = os_popen(cmd);
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
int64_t os_capture_len(OsCap *c) { return c ? c->len : 0; }
int os_capture_code(OsCap *c) { return c ? c->code : -1; }
void os_capture_free(OsCap *c) {
    if (!c) return;
    free(c->data);
    free(c);
}
