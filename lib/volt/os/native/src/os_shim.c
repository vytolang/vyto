/* volt/os native backing — environment, working directory, process info, and
   subprocess execution.

   Fixed-size queries (cwd, hostname, arch) fill a caller `bytes(cap)` buffer
   and NUL-terminate. `os_capture` uses the opaque-handle + explicit-free shape
   (mirroring net_shim.c): the buffer it mallocs is owned by a Volt ProcResult
   whose deinit calls os_capture_free exactly once. */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <sys/wait.h>

const char *os_getenv(const char *k) { return getenv(k); }
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
    struct utsname u;
    if (uname(&u) != 0) { if (cap > 0) buf[0] = 0; return 0; }
    strncpy(buf, u.machine, (size_t)cap - 1);
    buf[cap - 1] = 0;
    return (int)strlen(buf);
}

/* Run through the shell; return the child's exit status (or -1 if it could not
   be launched / did not exit normally). */
int os_run(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
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
    c->code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
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
