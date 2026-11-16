/* vyto/desktop/print — the printers installed in the OS, and raw printing
 * through their queues. See ../../README.md.
 *
 * Raw means the bytes reach the printer untouched: an app that speaks the
 * printer's own language (ESC/POS for a receipt printer, ZPL for a label
 * printer) uses the OS queue only as the connection. Nothing is drawn or
 * rendered here.
 *
 * Windows: winspool. EnumPrintersW / GetDefaultPrinterW to list,
 * OpenPrinterW + GetPrinterW (level 2) for a printer's state,
 * StartDocPrinterW with datatype "RAW" + WritePrinter to print, GetJobW and
 * SetJobW (JOB_CONTROL_DELETE) for a job.
 *
 * Linux and macOS: the CUPS command-line programs lpstat, lp and cancel,
 * started from an argv array, never a shell, with LC_ALL=C so their output
 * parses the same in every locale, and with a time limit: a print system
 * that hangs must not hang the app. VYTO_PRINT_BIN_DIR names a directory to
 * take the three programs from instead of PATH (the tests' fakes).
 *
 * Freestanding and Android: stub arm, no printers, every call fails.
 *
 * Strings returned to Vyto are owned here: Vyto copies them with str() at
 * once. The winspool functions used are NOT yet in the audited Windows 7
 * export list (lib/vyto/win/win7_exports_provisional.txt holds them until a
 * Win7 dump replaces it). */
#include <stdint.h>

/* printer states and job states: print.vt mirrors these */
#define PS_READY 0
#define PS_BUSY 1
#define PS_OFFLINE 2
#define PS_ERROR 3
#define PS_PAPER_OUT 4
#define PS_PAUSED 5
#define PS_UNKNOWN 6
#define PS_MISSING 7
#define PS_UNAVAILABLE 8

#define JOB_QUEUED 0
#define JOB_PRINTING 1
#define JOB_DONE 2
#define JOB_ERROR 3
#define JOB_UNKNOWN 4

#if !defined(VT_NO_LIBC) && !defined(__ANDROID__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the list handle, shared by both hosted arms ---- */

typedef struct {
    char *name;
    char *port;
    int32_t is_default;
    int32_t state;
} VpPrinter;

typedef struct {
    VpPrinter *v;
    int n, cap;
} VpList;

static char g_err[512];

static void vp_set_err(const char *m) { snprintf(g_err, sizeof g_err, "%s", m ? m : ""); }

static char *vp_dup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static VpPrinter *vp_add(VpList *l, const char *name) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        VpPrinter *nv = (VpPrinter *)realloc(l->v, sizeof(VpPrinter) * (size_t)nc);
        if (!nv) return NULL;
        l->v = nv;
        l->cap = nc;
    }
    VpPrinter *p = &l->v[l->n++];
    p->name = vp_dup(name);
    p->port = vp_dup("");
    p->is_default = 0;
    p->state = PS_UNKNOWN;
    return p;
}

static VpPrinter *vp_find(VpList *l, const char *name) {
    for (int i = 0; i < l->n; i++) if (l->v[i].name && strcmp(l->v[i].name, name) == 0) return &l->v[i];
    return NULL;
}

int32_t vprint_count(void *h) { return h ? (int32_t)((VpList *)h)->n : 0; }
const char *vprint_name(void *h, int32_t i) {
    VpList *l = (VpList *)h;
    if (!l || i < 0 || i >= l->n || !l->v[i].name) return "";
    return l->v[i].name;
}
const char *vprint_port(void *h, int32_t i) {
    VpList *l = (VpList *)h;
    if (!l || i < 0 || i >= l->n || !l->v[i].port) return "";
    return l->v[i].port;
}
int32_t vprint_is_default(void *h, int32_t i) {
    VpList *l = (VpList *)h;
    if (!l || i < 0 || i >= l->n) return 0;
    return l->v[i].is_default;
}
int32_t vprint_pstate(void *h, int32_t i) {
    VpList *l = (VpList *)h;
    if (!l || i < 0 || i >= l->n) return PS_UNKNOWN;
    return l->v[i].state;
}
void vprint_free(void *h) {
    VpList *l = (VpList *)h;
    if (!l) return;
    for (int i = 0; i < l->n; i++) { free(l->v[i].name); free(l->v[i].port); }
    free(l->v);
    free(l);
}
const char *vprint_error(void) { return g_err; }

#endif

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#include <windows.h>
#include <winspool.h>
#include "vyto_win.h"

int32_t vprint_available(void) { return 1; }

static int32_t vp_state_of(DWORD status, DWORD attrs) {
    if ((attrs & PRINTER_ATTRIBUTE_WORK_OFFLINE) || (status & PRINTER_STATUS_OFFLINE)) return PS_OFFLINE;
    if (status & (PRINTER_STATUS_PAPER_OUT | PRINTER_STATUS_PAPER_JAM | PRINTER_STATUS_PAPER_PROBLEM)) return PS_PAPER_OUT;
    if (status & (PRINTER_STATUS_PAUSED | PRINTER_STATUS_PENDING_DELETION)) return PS_PAUSED;
    if (status & (PRINTER_STATUS_ERROR | PRINTER_STATUS_NOT_AVAILABLE | PRINTER_STATUS_DOOR_OPEN |
                  PRINTER_STATUS_USER_INTERVENTION | PRINTER_STATUS_NO_TONER | PRINTER_STATUS_OUTPUT_BIN_FULL |
                  PRINTER_STATUS_SERVER_UNKNOWN))
        return PS_ERROR;
    if (status & (PRINTER_STATUS_PRINTING | PRINTER_STATUS_BUSY | PRINTER_STATUS_PROCESSING |
                  PRINTER_STATUS_WARMING_UP | PRINTER_STATUS_INITIALIZING))
        return PS_BUSY;
    return PS_READY;
}

static void vp_win_err(const char *what) {
    DWORD e = GetLastError();
    char msg[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
    size_t n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == '.')) msg[--n] = 0;
    snprintf(g_err, sizeof g_err, "%s: %s (%lu)", what, msg, (unsigned long)e);
}

void *vprint_printers(void) {
    VpList *l = (VpList *)calloc(1, sizeof(VpList));
    if (!l) return NULL;
    DWORD need = 0, count = 0;
    DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, NULL, 2, NULL, 0, &need, &count);
    if (need == 0) return l;
    BYTE *buf = (BYTE *)malloc(need);
    if (!buf) return l;
    if (!EnumPrintersW(flags, NULL, 2, buf, need, &need, &count)) {
        vp_win_err("EnumPrinters");
        free(buf);
        return l;
    }
    wchar_t defname[512];
    DWORD dn = 512;
    int has_default = GetDefaultPrinterW(defname, &dn) ? 1 : 0;
    PRINTER_INFO_2W *pi = (PRINTER_INFO_2W *)buf;
    for (DWORD i = 0; i < count; i++) {
        char *name = vt_utf8_from_w(pi[i].pPrinterName);
        if (!name) continue;
        VpPrinter *p = vp_add(l, name);
        free(name);
        if (!p) break;
        free(p->port);
        p->port = vt_utf8_from_w(pi[i].pPortName);
        p->state = vp_state_of(pi[i].Status, pi[i].Attributes);
        p->is_default = has_default && wcscmp(defname, pi[i].pPrinterName) == 0;
    }
    free(buf);
    return l;
}

static HANDLE vp_open(const char *name) {
    VtWBuf w;
    vt_wbuf_from(&w, name);
    HANDLE h = NULL;
    if (!OpenPrinterW(w.p, &h, NULL)) h = NULL;
    vt_wbuf_free(&w);
    return h;
}

int32_t vprint_state(const char *name) {
    HANDLE h = vp_open(name);
    if (!h) { vp_win_err("OpenPrinter"); return PS_MISSING; }
    DWORD need = 0;
    GetPrinterW(h, 2, NULL, 0, &need);
    int32_t st = PS_UNKNOWN;
    BYTE *buf = need ? (BYTE *)malloc(need) : NULL;
    if (buf && GetPrinterW(h, 2, buf, need, &need)) {
        PRINTER_INFO_2W *pi = (PRINTER_INFO_2W *)buf;
        st = vp_state_of(pi->Status, pi->Attributes);
    } else {
        vp_win_err("GetPrinter");
    }
    free(buf);
    ClosePrinter(h);
    return st;
}

int32_t vprint_raw(const char *name, const void *data, int64_t len, const char *title) {
    g_err[0] = 0;
    HANDLE h = vp_open(name);
    if (!h) { vp_win_err("OpenPrinter"); return -1; }
    VtWBuf wt;
    vt_wbuf_from(&wt, title);
    DOC_INFO_1W di;
    di.pDocName = wt.p;
    di.pOutputFile = NULL;
    di.pDatatype = (LPWSTR)L"RAW";
    DWORD job = StartDocPrinterW(h, 1, (LPBYTE)&di);
    vt_wbuf_free(&wt);
    if (job == 0) { vp_win_err("StartDocPrinter"); ClosePrinter(h); return -1; }
    int ok = StartPagePrinter(h) ? 1 : 0;
    const BYTE *p = (const BYTE *)data;
    int64_t left = len;
    while (ok && left > 0) {
        DWORD chunk = left > 65536 ? 65536 : (DWORD)left;
        DWORD wrote = 0;
        if (!WritePrinter(h, (LPVOID)p, chunk, &wrote) || wrote == 0) { vp_win_err("WritePrinter"); ok = 0; break; }
        p += wrote;
        left -= wrote;
    }
    if (ok) EndPagePrinter(h);
    EndDocPrinter(h);
    if (!ok) {
        /* a job half written must not print later */
        SetJobW(h, job, 0, NULL, JOB_CONTROL_DELETE);
        ClosePrinter(h);
        return -1;
    }
    ClosePrinter(h);
    return (int32_t)job;
}

int32_t vprint_job(const char *name, int32_t job) {
    HANDLE h = vp_open(name);
    if (!h) { vp_win_err("OpenPrinter"); return JOB_UNKNOWN; }
    DWORD need = 0;
    if (!GetJobW(h, (DWORD)job, 1, NULL, 0, &need) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        /* no such job: it left the queue */
        ClosePrinter(h);
        return JOB_DONE;
    }
    int32_t st = JOB_UNKNOWN;
    BYTE *buf = need ? (BYTE *)malloc(need) : NULL;
    if (buf && GetJobW(h, (DWORD)job, 1, buf, need, &need)) {
        DWORD s = ((JOB_INFO_1W *)buf)->Status;
        if (s & (JOB_STATUS_PRINTED | JOB_STATUS_COMPLETE)) st = JOB_DONE;
        else if (s & (JOB_STATUS_ERROR | JOB_STATUS_OFFLINE | JOB_STATUS_PAPEROUT | JOB_STATUS_BLOCKED_DEVQ |
                      JOB_STATUS_USER_INTERVENTION | JOB_STATUS_DELETING | JOB_STATUS_DELETED))
            st = JOB_ERROR;
        else if (s & JOB_STATUS_PRINTING) st = JOB_PRINTING;
        else st = JOB_QUEUED;
    } else {
        st = JOB_DONE;
    }
    free(buf);
    ClosePrinter(h);
    return st;
}

int32_t vprint_cancel(const char *name, int32_t job) {
    HANDLE h = vp_open(name);
    if (!h) { vp_win_err("OpenPrinter"); return 0; }
    int ok = SetJobW(h, (DWORD)job, 0, NULL, JOB_CONTROL_DELETE) ? 1 : 0;
    if (!ok) vp_win_err("SetJob");
    ClosePrinter(h);
    return ok;
}

#elif !defined(VT_NO_LIBC) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define VP_TIMEOUT_MS 5000

int32_t vprint_available(void) { return 1; }

static int64_t vp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The program to run: VYTO_PRINT_BIN_DIR/<prog> when set, else <prog> on PATH. */
static void vp_prog(const char *prog, char *out, size_t cap) {
    const char *dir = getenv("VYTO_PRINT_BIN_DIR");
    if (dir && *dir) snprintf(out, cap, "%s/%s", dir, prog);
    else snprintf(out, cap, "%s", prog);
}

/* The child's environment: ours with LC_ALL=C (and LANG=C) put first, so the
   CUPS programs answer in English whatever the user's locale. */
static char **vp_env(void) {
    int n = 0;
    while (environ && environ[n]) n++;
    char **e = (char **)malloc(sizeof(char *) * (size_t)(n + 3));
    if (!e) return NULL;
    int k = 0;
    e[k++] = (char *)"LC_ALL=C";
    e[k++] = (char *)"LANG=C";
    for (int i = 0; i < n; i++) {
        if (strncmp(environ[i], "LC_ALL=", 7) == 0 || strncmp(environ[i], "LANG=", 5) == 0) continue;
        e[k++] = environ[i];
    }
    e[k] = NULL;
    return e;
}

/* Run argv with stdin from /dev/null, stdout captured into *out (malloc'd,
   NUL-terminated), stderr appended to it too (CUPS reports errors there).
   Gives up after VP_TIMEOUT_MS: the child is killed and -2 returned. -1 when
   it could not start; otherwise its exit status. No shell, ever. */
static int vp_run(char *const argv[], char **out) {
    *out = NULL;
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    char **env = vp_env();
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env ? env : environ);
    posix_spawn_file_actions_destroy(&fa);
    free(env);
    close(pfd[1]);
    if (rc != 0) { close(pfd[0]); return -1; }
    size_t cap = 4096, n = 0;
    char *b = (char *)malloc(cap);
    int64_t deadline = vp_now_ms() + VP_TIMEOUT_MS;
    int timed_out = 0;
    for (;;) {
        int64_t left = deadline - vp_now_ms();
        if (left <= 0) { timed_out = 1; break; }
        struct pollfd pf = { pfd[0], POLLIN, 0 };
        int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) { timed_out = 1; break; }
        if (pr < 0) break;
        if (b && n + 1 >= cap) {
            char *nb = (char *)realloc(b, cap *= 2);
            if (!nb) { free(b); b = NULL; }
            else b = nb;
        }
        char sink[512];
        ssize_t r = b ? read(pfd[0], b + n, cap - n - 1) : read(pfd[0], sink, sizeof sink);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (b) n += (size_t)r;
    }
    close(pfd[0]);
    if (timed_out) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (b) b[n] = 0;
    *out = b;
    if (timed_out) return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* A CUPS queue name never starts with '-' and never holds a space, a slash,
   '#' or a control character; refusing those keeps a name from being read as
   an option or a URI part. */
static int vp_name_ok(const char *name) {
    if (!name || !*name || name[0] == '-') return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p <= ' ' || *p == '/' || *p == '#' || *p == 0x7f) return 0;
    }
    return 1;
}

static void vp_fail_run(int rc, const char *prog, const char *out) {
    if (rc == -1) snprintf(g_err, sizeof g_err, "%s could not be started (is CUPS installed?)", prog);
    else if (rc == -2) snprintf(g_err, sizeof g_err, "%s did not answer in time", prog);
    else {
        /* the first line of what it said */
        char line[400] = "";
        if (out) {
            size_t k = 0;
            while (out[k] && out[k] != '\n' && k < sizeof line - 1) { line[k] = out[k]; k++; }
            line[k] = 0;
        }
        snprintf(g_err, sizeof g_err, "%s failed: %s", prog, line[0] ? line : "no message");
    }
}

/* "printer NAME is idle.  enabled since ..." / "... now printing NAME-3." /
   "printer NAME disabled since ... -" followed by indented reason lines. */
static int32_t vp_state_from_line(const char *rest) {
    if (strstr(rest, "disabled")) return PS_PAUSED;
    if (strstr(rest, "now printing")) return PS_BUSY;
    if (strstr(rest, "is idle")) return PS_READY;
    return PS_UNKNOWN;
}

void *vprint_printers(void) {
    g_err[0] = 0;
    VpList *l = (VpList *)calloc(1, sizeof(VpList));
    if (!l) return NULL;
    char prog[1024];
    vp_prog("lpstat", prog, sizeof prog);
    char *out = NULL;
    char *av1[] = { prog, (char *)"-p", NULL };
    int rc = vp_run(av1, &out);
    if (rc < 0) { vp_fail_run(rc, "lpstat", out); free(out); return l; }
    /* "No destinations added." exits non-zero: an empty list, not an error */
    for (char *ln = out ? strtok(out, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        if (strncmp(ln, "printer ", 8) != 0) continue;
        char *name = ln + 8;
        char *sp = strchr(name, ' ');
        if (!sp) continue;
        *sp = 0;
        VpPrinter *p = vp_add(l, name);
        if (p) p->state = vp_state_from_line(sp + 1);
    }
    free(out);
    if (l->n == 0) return l;
    char *av2[] = { prog, (char *)"-d", NULL };
    if (vp_run(av2, &out) == 0 && out) {
        const char *k = "system default destination: ";
        char *d = strstr(out, k);
        if (d) {
            d += strlen(k);
            char *e = strchr(d, '\n');
            if (e) *e = 0;
            VpPrinter *p = vp_find(l, d);
            if (p) p->is_default = 1;
        }
    }
    free(out);
    char *av3[] = { prog, (char *)"-v", NULL };
    if (vp_run(av3, &out) == 0 && out) {
        for (char *ln = strtok(out, "\n"); ln; ln = strtok(NULL, "\n")) {
            if (strncmp(ln, "device for ", 11) != 0) continue;
            char *name = ln + 11;
            char *c = strstr(name, ": ");
            if (!c) continue;
            *c = 0;
            VpPrinter *p = vp_find(l, name);
            if (p) { free(p->port); p->port = vp_dup(c + 2); }
        }
    }
    free(out);
    return l;
}

int32_t vprint_state(const char *name) {
    g_err[0] = 0;
    if (!vp_name_ok(name)) { vp_set_err("not a printer name"); return PS_MISSING; }
    char prog[1024];
    vp_prog("lpstat", prog, sizeof prog);
    char *out = NULL;
    char *av[] = { prog, (char *)"-l", (char *)"-p", (char *)name, NULL };
    int rc = vp_run(av, &out);
    if (rc < 0) { vp_fail_run(rc, "lpstat", out); free(out); return PS_UNKNOWN; }
    if (rc != 0 || !out || strncmp(out, "printer ", 8) != 0) {
        vp_fail_run(1, "lpstat", out);
        free(out);
        return PS_MISSING;
    }
    int32_t st = vp_state_from_line(out);
    /* the alerts say more than idle/disabled: CUPS reports a USB printer
       that is unplugged or out of paper here */
    const char *al = strstr(out, "Alerts:");
    if (al) {
        const char *e = strchr(al, '\n');
        size_t n = e ? (size_t)(e - al) : strlen(al);
        char alerts[256];
        if (n >= sizeof alerts) n = sizeof alerts - 1;
        memcpy(alerts, al, n);
        alerts[n] = 0;
        if (strstr(alerts, "media-empty") || strstr(alerts, "media-needed")) st = PS_PAPER_OUT;
        else if (strstr(alerts, "offline")) st = PS_OFFLINE;
        else if (st != PS_PAUSED && (strstr(alerts, "-error") || strstr(alerts, "door-open") || strstr(alerts, "cover-open")))
            st = PS_ERROR;
    }
    free(out);
    if (st == PS_READY || st == PS_BUSY) {
        /* a queue that is not accepting jobs takes none: an error to the app */
        char *av2[] = { prog, (char *)"-a", (char *)name, NULL };
        if (vp_run(av2, &out) == 0 && out && strstr(out, "not accepting")) st = PS_ERROR;
        free(out);
    }
    return st;
}

int32_t vprint_raw(const char *name, const void *data, int64_t len, const char *title) {
    g_err[0] = 0;
    if (!vp_name_ok(name)) { vp_set_err("not a printer name"); return -1; }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char path[1024];
    snprintf(path, sizeof path, "%s/vyto-print-XXXXXX", tmp);
    int fd = mkstemp(path);
    if (fd < 0) { vp_set_err("could not write a temporary file"); return -1; }
    const char *p = (const char *)data;
    int64_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, (size_t)left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); unlink(path); vp_set_err("could not write a temporary file"); return -1; }
        p += w;
        left -= w;
    }
    close(fd);
    char prog[1024];
    vp_prog("lp", prog, sizeof prog);
    /* "-dNAME" and "-tTITLE" as one element each: a value that starts with
       '-' cannot be read as another option, and "--" ends the options
       before the file */
    size_t nl = strlen(name) + 3, tl = strlen(title ? title : "") + 3;
    char *dopt = (char *)malloc(nl), *topt = (char *)malloc(tl);
    if (!dopt || !topt) { free(dopt); free(topt); unlink(path); vp_set_err("out of memory"); return -1; }
    snprintf(dopt, nl, "-d%s", name);
    snprintf(topt, tl, "-t%s", title ? title : "");
    char *av[] = { prog, dopt, (char *)"-o", (char *)"raw", topt, (char *)"--", path, NULL };
    char *out = NULL;
    int rc = vp_run(av, &out);
    unlink(path);
    free(dopt);
    free(topt);
    if (rc != 0) { vp_fail_run(rc, "lp", out); free(out); return -1; }
    /* "request id is NAME-12 (1 file(s))" */
    int32_t job = -1;
    const char *k = "request id is ";
    char *r = out ? strstr(out, k) : NULL;
    if (r) {
        r += strlen(k);
        char *e = strstr(r, " (");
        if (!e) e = strchr(r, '\n');
        if (e) {
            char *dash = NULL;
            for (char *q = r; q < e; q++) if (*q == '-') dash = q;
            if (dash) job = (int32_t)atol(dash + 1);
        }
    }
    if (job <= 0) vp_set_err("lp did not say which job it queued");
    free(out);
    return job;
}

int32_t vprint_job(const char *name, int32_t job) {
    g_err[0] = 0;
    if (!vp_name_ok(name)) { vp_set_err("not a printer name"); return JOB_UNKNOWN; }
    char prog[1024];
    vp_prog("lpstat", prog, sizeof prog);
    char *out = NULL;
    char *av[] = { prog, (char *)"-W", (char *)"not-completed", (char *)"-o", (char *)name, NULL };
    int rc = vp_run(av, &out);
    if (rc < 0) { vp_fail_run(rc, "lpstat", out); free(out); return JOB_UNKNOWN; }
    char id[600];
    snprintf(id, sizeof id, "%s-%d", name, (int)job);
    size_t il = strlen(id);
    int32_t st = JOB_DONE;
    for (char *ln = out ? strtok(out, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        if (strncmp(ln, id, il) == 0 && (ln[il] == ' ' || ln[il] == '\t' || ln[il] == 0)) { st = JOB_QUEUED; break; }
    }
    free(out);
    if (st == JOB_QUEUED) {
        /* "printer NAME now printing NAME-12." */
        char *av2[] = { prog, (char *)"-p", (char *)name, NULL };
        if (vp_run(av2, &out) == 0 && out) {
            char *np = strstr(out, "now printing ");
            if (np && strncmp(np + 13, id, il) == 0) st = JOB_PRINTING;
        }
        free(out);
    }
    return st;
}

int32_t vprint_cancel(const char *name, int32_t job) {
    g_err[0] = 0;
    if (!vp_name_ok(name)) { vp_set_err("not a printer name"); return 0; }
    char prog[1024];
    vp_prog("cancel", prog, sizeof prog);
    char id[600];
    snprintf(id, sizeof id, "%s-%d", name, (int)job);
    char *out = NULL;
    char *av[] = { prog, id, NULL };
    int rc = vp_run(av, &out);
    if (rc != 0) { vp_fail_run(rc, "cancel", out); free(out); return 0; }
    free(out);
    return 1;
}

#else /* freestanding or Android: no printing system */

int32_t vprint_available(void) { return 0; }
void *vprint_printers(void) { return 0; }
int32_t vprint_count(void *h) { (void)h; return 0; }
const char *vprint_name(void *h, int32_t i) { (void)h; (void)i; return ""; }
const char *vprint_port(void *h, int32_t i) { (void)h; (void)i; return ""; }
int32_t vprint_is_default(void *h, int32_t i) { (void)h; (void)i; return 0; }
int32_t vprint_pstate(void *h, int32_t i) { (void)h; (void)i; return PS_UNAVAILABLE; }
void vprint_free(void *h) { (void)h; }
const char *vprint_error(void) { return "printing is not available on this platform"; }
int32_t vprint_state(const char *name) { (void)name; return PS_UNAVAILABLE; }
int32_t vprint_raw(const char *name, const void *data, int64_t len, const char *title) {
    (void)name; (void)data; (void)len; (void)title;
    return -1;
}
int32_t vprint_job(const char *name, int32_t job) { (void)name; (void)job; return JOB_UNKNOWN; }
int32_t vprint_cancel(const char *name, int32_t job) { (void)name; (void)job; return 0; }

#endif
