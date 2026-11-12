/* vyto/desktop — the OS's own file dialogs, message boxes, "open this", and
 * per-user folders. See ../../README.md.
 *
 * Three arms, one set of entry points:
 *
 *   Windows       comdlg32 / IFileOpenDialog / MessageBoxW / ShellExecuteW /
 *                 SHGetKnownFolderPath. Every function is in the audited
 *                 Windows 7 export list (lib/vyto/win/win7_exports.txt).
 *   POSIX         runs zenity or kdialog, and xdg-open — as an argv array via
 *                 posix_spawnp, NEVER through a shell. A title or path is
 *                 user data and reaches the program byte for byte; there is no
 *                 string a caller can pass that runs a command.
 *   stub          freestanding (VT_NO_LIBC) and Android: every entry point
 *                 answers "unavailable". Android apps use vyto/mobile/android.
 *                 Includes nothing, so a --freestanding build of a program that
 *                 merely imports vyto/desktop still compiles.
 *
 * Results come back through an opaque handle (count + indexed paths + free)
 * rather than one joined string, so a path may hold any byte but NUL. C owns
 * every returned string; Vyto copies with str() at once.
 *
 * Dialogs are modal and block the calling thread until the person answers. */
#include <stdint.h>

/* Result status codes, shared with desktop.vt. */
#define VD_OK          0
#define VD_CANCELLED   1
#define VD_UNAVAILABLE 2

/* Dialog kinds and message kinds, shared with desktop.vt. */
#define VD_OPEN      0
#define VD_OPEN_MANY 1
#define VD_SAVE      2
#define VD_FOLDER    3

#define VD_INFO     0
#define VD_WARN     1
#define VD_ERROR    2
#define VD_QUESTION 3

#if !defined(VT_NO_LIBC) && !defined(__ANDROID__)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int status;
    int n;
    char **paths;
} VdResult;

static VdResult *vd_result(int status) {
    VdResult *r = calloc(1, sizeof *r);
    if (r) r->status = status;
    return r;
}

static void vd_push(VdResult *r, const char *s, size_t len) {
    char **np = realloc(r->paths, sizeof *np * (size_t)(r->n + 1));
    if (!np) return;
    r->paths = np;
    char *c = malloc(len + 1);
    if (!c) return;
    memcpy(c, s, len);
    c[len] = 0;
    r->paths[r->n++] = c;
}

int32_t vdesk_result_status(void *h) { return h ? ((VdResult *)h)->status : VD_UNAVAILABLE; }
int32_t vdesk_result_count(void *h) { return h ? ((VdResult *)h)->n : 0; }
const char *vdesk_result_path(void *h, int32_t i) {
    VdResult *r = h;
    return r && i >= 0 && i < r->n ? r->paths[i] : "";
}
void vdesk_result_free(void *h) {
    VdResult *r = h;
    if (!r) return;
    for (int i = 0; i < r->n; i++) free(r->paths[i]);
    free(r->paths);
    free(r);
}

/* Directory answers: a small ring of buffers, the os_wgetenv pattern, so a
   caller holding the previous answer is not overwritten by the next. */
static char g_dir_ring[4][4096];
static int g_dir_next;
static const char *vd_dir_out(const char *s) {
    char *b = g_dir_ring[g_dir_next++ & 3];
    size_t n = strlen(s);
    if (n >= sizeof g_dir_ring[0]) return "";
    memcpy(b, s, n + 1);
    return b;
}

#endif

/* ======================================================================== */
#if defined(_WIN32) && !defined(VT_NO_LIBC)

#define COBJMACROS
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include "vyto_win.h"

/* Surface.native_window() hands the HWND over as a C unsigned long, which is
   32-bit on Win64. Window handles are 32-bit values sign-extended by design
   (that is what lets 32- and 64-bit processes share them), so sign-extending
   back is exact. 0 means "no owner". */
static HWND vd_owner(unsigned long w) { return w ? (HWND)(LONG_PTR)(LONG)w : NULL; }

int32_t vdesk_available(void) { return 1; }

/* "Images\t*.png;*.jpg\nText\t*.txt" -> the double-NUL-terminated wide list
   OPENFILENAME wants: L"Images\0*.png;*.jpg\0Text\0*.txt\0\0". */
static wchar_t *vd_filter_w(const char *spec) {
    if (!spec || !*spec) return NULL;
    VtWBuf w;
    if (!vt_wbuf_from(&w, spec)) { vt_wbuf_free(&w); return NULL; }
    size_t n = wcslen(w.p);
    wchar_t *out = malloc((n + 2) * sizeof *out);
    if (out) {
        for (size_t i = 0; i < n; i++) out[i] = (w.p[i] == L'\t' || w.p[i] == L'\n') ? 0 : w.p[i];
        out[n] = 0;
        out[n + 1] = 0;
    }
    vt_wbuf_free(&w);
    return out;
}

static void vd_push_w(VdResult *r, const wchar_t *w, int n) {
    char *u = vt_utf8_from_wn(w, n);
    if (u) { vd_push(r, u, strlen(u)); free(u); }
}

static void vd_com_init(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    (void)hr; /* S_FALSE (already) and RPC_E_CHANGED_MODE (MTA thread) both work for a modal dialog */
}

static VdResult *vd_folder(const char *title, const char *start, HWND owner) {
    vd_com_init();
    IFileOpenDialog *d = NULL;
    VdResult *r = vd_result(VD_UNAVAILABLE);
    if (!r) return NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&d)) || !d)
        return r;
    DWORD opts = 0;
    IFileOpenDialog_GetOptions(d, &opts);
    IFileOpenDialog_SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (title && *title) {
        VtWBuf t;
        if (vt_wbuf_from(&t, title)) IFileOpenDialog_SetTitle(d, t.p);
        vt_wbuf_free(&t);
    }
    if (start && *start) {
        VtWBuf s;
        IShellItem *it = NULL;
        if (vt_wbuf_from(&s, start) &&
            SUCCEEDED(SHCreateItemFromParsingName(s.p, NULL, &IID_IShellItem, (void **)&it)) && it) {
            IFileOpenDialog_SetFolder(d, it);
            IShellItem_Release(it);
        }
        vt_wbuf_free(&s);
    }
    HRESULT hr = IFileOpenDialog_Show(d, owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        r->status = VD_CANCELLED;
    } else if (SUCCEEDED(hr)) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it)) && it) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p)) && p) {
                vd_push_w(r, p, -1);
                CoTaskMemFree(p);
                r->status = VD_OK;
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
    return r;
}

void *vdesk_dialog(int32_t kind, const char *title, const char *start, const char *filters,
                   unsigned long owner_id) {
    HWND owner = vd_owner(owner_id);
    if (kind == VD_FOLDER) return vd_folder(title, start, owner);
    VdResult *r = vd_result(VD_UNAVAILABLE);
    if (!r) return NULL;

    enum { CAP = 32768 };  /* the documented OPENFILENAME maximum */
    wchar_t *buf = calloc(CAP, sizeof *buf);
    wchar_t *filt = vd_filter_w(filters);
    VtWBuf t, s;
    vt_wbuf_from(&t, title ? title : "");
    vt_wbuf_from(&s, start ? start : "");
    if (!buf) goto out;

    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = owner;
    o.lpstrFilter = filt;
    o.nFilterIndex = filt ? 1 : 0;
    o.lpstrFile = buf;
    o.nMaxFile = CAP;
    o.lpstrTitle = (title && *title) ? t.p : NULL;
    o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    /* `start` is a folder for open dialogs and a suggested file (possibly
       with a path) for save. */
    if (kind == VD_SAVE) {
        if (start && *start) wcsncpy(buf, s.p, CAP - 1);
        o.Flags |= OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    } else {
        if (start && *start) o.lpstrInitialDir = s.p;
        o.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (kind == VD_OPEN_MANY) o.Flags |= OFN_ALLOWMULTISELECT;
    }
    BOOL ok = kind == VD_SAVE ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
    if (!ok) {
        /* 0 from CommDlgExtendedError is the person pressing Cancel. */
        r->status = CommDlgExtendedError() == 0 ? VD_CANCELLED : VD_UNAVAILABLE;
        goto out;
    }
    r->status = VD_OK;
    if (kind == VD_OPEN_MANY && o.nFileOffset > 0 && buf[o.nFileOffset - 1] == 0) {
        /* several files: "dir\0name1\0name2\0\0" */
        size_t dl = wcslen(buf);
        for (wchar_t *p = buf + dl + 1; *p; p += wcslen(p) + 1) {
            size_t pl = wcslen(p);
            wchar_t *full = malloc((dl + 1 + pl + 1) * sizeof *full);
            if (!full) break;
            memcpy(full, buf, dl * sizeof *full);
            full[dl] = L'\\';
            memcpy(full + dl + 1, p, (pl + 1) * sizeof *full);
            vd_push_w(r, full, -1);
            free(full);
        }
    } else {
        vd_push_w(r, buf, -1);
    }
out:
    free(buf);
    free(filt);
    vt_wbuf_free(&t);
    vt_wbuf_free(&s);
    return r;
}

int32_t vdesk_message(int32_t kind, const char *title, const char *text, unsigned long owner_id) {
    UINT flags = MB_OK | MB_ICONINFORMATION;
    if (kind == VD_WARN) flags = MB_OK | MB_ICONWARNING;
    else if (kind == VD_ERROR) flags = MB_OK | MB_ICONERROR;
    else if (kind == VD_QUESTION) flags = MB_YESNO | MB_ICONQUESTION;
    VtWBuf t, x;
    vt_wbuf_from(&t, title ? title : "");
    vt_wbuf_from(&x, text ? text : "");
    HWND owner = vd_owner(owner_id);
    /* With no owner, MB_TASKMODAL keeps it modal to this thread's windows. */
    int r = MessageBoxW(owner, x.p, t.p, flags | (owner ? 0 : MB_TASKMODAL) | MB_SETFOREGROUND);
    vt_wbuf_free(&t);
    vt_wbuf_free(&x);
    if (r == 0) return -1;
    return (r == IDOK || r == IDYES) ? 1 : 0;
}

int32_t vdesk_open(const char *target) {
    if (!target || !*target) return 0;
    VtWBuf w;
    if (!vt_wbuf_from(&w, target)) { vt_wbuf_free(&w); return 0; }
    vd_com_init(); /* ShellExecute may hand off to a COM-based handler */
    HINSTANCE h = ShellExecuteW(NULL, L"open", w.p, NULL, NULL, SW_SHOWNORMAL);
    vt_wbuf_free(&w);
    return (INT_PTR)h > 32 ? 1 : 0;
}

const char *vdesk_dir(int32_t which, const char *app) {
    const KNOWNFOLDERID *id = which == 0 ? &FOLDERID_RoamingAppData : &FOLDERID_LocalAppData;
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(id, 0, NULL, &base)) || !base) return "";
    char *u = vt_utf8_from_w(base);
    CoTaskMemFree(base);
    if (!u) return "";
    char tmp[4096];
    int n = snprintf(tmp, sizeof tmp, which == 2 ? "%s\\%s\\cache" : "%s\\%s", u, app);
    free(u);
    if (n < 0 || (size_t)n >= sizeof tmp) return "";
    return vd_dir_out(tmp);
}

/* ======================================================================== */
#elif !defined(VT_NO_LIBC) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Is `name` an executable on PATH (or, with a '/', at that path)? Writes the
   resolved path to out. */
static int vd_which(const char *name, char *out, size_t cap) {
    if (strchr(name, '/')) {
        if (access(name, X_OK) != 0) return 0;
        snprintf(out, cap, "%s", name);
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    while (*path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        if (l > 0 && l + strlen(name) + 2 < cap) {
            memcpy(out, path, l);
            out[l] = '/';
            strcpy(out + l + 1, name);
            struct stat st;
            if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0) return 1;
        }
        path = e ? e + 1 : path + l;
    }
    return 0;
}

/* Run argv (argv[0] resolved through PATH), stdin from /dev/null, stderr
   discarded (GTK and Qt are chatty there), stdout captured. Returns the exit
   status, or -1 when it could not be started; *out is malloc'd. No shell is
   involved at any point. */
static int vd_run(char *const argv[], char **out, size_t *outlen) {
    int pfd[2];
    *out = NULL;
    *outlen = 0;
    if (pipe(pfd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    posix_spawn_file_actions_addclose(&fa, pfd[1]);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (rc != 0) { close(pfd[0]); return -1; }
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    for (;;) {
        if (!b) break;
        if (n + 1 >= cap) {
            char *nb = realloc(b, cap *= 2);
            if (!nb) { free(b); b = NULL; break; }
            b = nb;
        }
        ssize_t r = read(pfd[0], b + n, cap - n - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(pfd[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (b) b[n] = 0;
    *out = b;
    *outlen = n;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Start argv detached and do not wait: xdg-open may run the handler in the
   foreground. The double fork hands the grandchild to init, so nothing is
   left as a zombie of the app. Returns 1 when it was started. */
static int vd_spawn_detached(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        setsid();
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t g;
        int rc = posix_spawnp(&g, argv[0], &fa, NULL, argv, environ);
        _exit(rc == 0 ? 0 : 1);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* ---- which dialog program ---- */

enum { BK_NONE, BK_ZENITY, BK_KDIALOG };
static int g_backend = -1;
static char g_backend_path[4096];
static int g_zenity_major;

static void vd_pick_backend(void) {
    if (g_backend >= 0) return;
    g_backend = BK_NONE;
    const char *forced = getenv("VYTO_DESKTOP_DIALOG");
    char p[4096];
    if (forced && *forced) {
        /* A test or a user naming the program outright. Its flavour comes
           from its name; anything that is not a kdialog speaks zenity. */
        if (vd_which(forced, p, sizeof p)) {
            snprintf(g_backend_path, sizeof g_backend_path, "%s", p);
            const char *b = strrchr(p, '/');
            g_backend = strstr(b ? b + 1 : p, "kdialog") ? BK_KDIALOG : BK_ZENITY;
        }
    } else {
        const char *de = getenv("XDG_CURRENT_DESKTOP");
        int kde = de && strstr(de, "KDE");
        if (kde && vd_which("kdialog", p, sizeof p)) g_backend = BK_KDIALOG;
        else if (vd_which("zenity", p, sizeof p)) g_backend = BK_ZENITY;
        else if (vd_which("kdialog", p, sizeof p)) g_backend = BK_KDIALOG;
        if (g_backend != BK_NONE) snprintf(g_backend_path, sizeof g_backend_path, "%s", p);
    }
    if (g_backend == BK_ZENITY) {
        /* zenity 4 (GTK 4) dropped --attach and --confirm-overwrite, and
           refuses unknown options outright, so the version decides the flags. */
        char *argv[] = { g_backend_path, "--version", NULL };
        char *o = NULL;
        size_t ol = 0;
        if (vd_run(argv, &o, &ol) == 0 && o) g_zenity_major = atoi(o);
        free(o);
        if (g_zenity_major <= 0) g_zenity_major = 3;
    }
}

int32_t vdesk_available(void) {
    vd_pick_backend();
    return g_backend != BK_NONE;
}

/* argv builder: every element is its own malloc'd string, so a value is
   never split, joined, quoted or interpreted. */
typedef struct { char *v[64]; int n; } Argv;

static void av_add(Argv *a, const char *s) {
    if (a->n < 63) {
        size_t l = strlen(s) + 1;
        char *c = malloc(l);
        if (c) { memcpy(c, s, l); a->v[a->n++] = c; }
    }
    a->v[a->n] = NULL;
}
static void av_addf(Argv *a, const char *opt, const char *val) {
    /* "--opt=value" as ONE element: a value starting with '-' can then never
       be read as an option by either program's parser. */
    size_t l = strlen(opt) + 1 + strlen(val) + 1;
    char *c = malloc(l);
    if (!c) return;
    snprintf(c, l, "%s=%s", opt, val);
    av_add(a, c);
    free(c);
}
static void av_free(Argv *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    a->n = 0;
}

/* A positional path for kdialog: one starting with '-' gets "./" so it is a
   path, not an option. */
static void av_add_path(Argv *a, const char *p) {
    if (p[0] == '-') {
        size_t l = strlen(p) + 3;
        char *c = malloc(l);
        if (!c) return;
        snprintf(c, l, "./%s", p);
        av_add(a, c);
        free(c);
    } else {
        av_add(a, p);
    }
}

/* filters: "Name\t*.a;*.b\nName2\t*.c" (built by desktop.vt) -> each
   program's own form. Zenity: one --file-filter="Name | *.a *.b" per filter.
   kdialog: one string, "Name (*.a *.b)" lines joined by '\n'. */
static void vd_filters(Argv *a, const char *filters, char *kd, size_t kdcap) {
    if (kd && kdcap) kd[0] = 0;
    if (!filters || !*filters) return;
    const char *p = filters;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[1024];
        if (l < sizeof line) {
            memcpy(line, p, l);
            line[l] = 0;
            char *tab = strchr(line, '\t');
            if (tab) {
                *tab = 0;
                for (char *q = tab + 1; *q; q++) if (*q == ';') *q = ' ';
                char one[1100];
                if (g_backend == BK_ZENITY) {
                    snprintf(one, sizeof one, "%s | %s", line, tab + 1);
                    av_addf(a, "--file-filter", one);
                } else if (kd) {
                    snprintf(one, sizeof one, "%s%s (%s)", kd[0] ? "\n" : "", line, tab + 1);
                    if (strlen(kd) + strlen(one) + 1 < kdcap) strcat(kd, one);
                }
            }
        }
        p = e ? e + 1 : p + l;
    }
}

void *vdesk_dialog(int32_t kind, const char *title, const char *start, const char *filters,
                   unsigned long owner) {
    vd_pick_backend();
    if (g_backend == BK_NONE) return vd_result(VD_UNAVAILABLE);
    if (!title) title = "";
    if (!start) start = "";
    Argv a = { {0}, 0 };
    av_add(&a, g_backend_path);
    char kd[4096];
    char xid[32];
    snprintf(xid, sizeof xid, "%lu", owner);
    if (g_backend == BK_ZENITY) {
        av_add(&a, "--file-selection");
        if (*title) av_addf(&a, "--title", title);
        if (kind == VD_SAVE) {
            av_add(&a, "--save");
            if (g_zenity_major < 4) av_add(&a, "--confirm-overwrite");
        }
        if (kind == VD_FOLDER) av_add(&a, "--directory");
        if (kind == VD_OPEN_MANY) { av_add(&a, "--multiple"); av_add(&a, "--separator=\n"); }
        if (*start) {
            /* zenity selects a file with --filename, or opens a folder when
               the value ends in '/' */
            size_t l = strlen(start);
            if (kind != VD_SAVE && start[l - 1] != '/') {
                char *s = malloc(l + 2);
                if (s) { snprintf(s, l + 2, "%s/", start); av_addf(&a, "--filename", s); free(s); }
            } else {
                av_addf(&a, "--filename", start);
            }
        }
        if (kind != VD_FOLDER) vd_filters(&a, filters, NULL, 0);
        if (owner && g_zenity_major < 4) av_addf(&a, "--attach", xid);
    } else {
        if (owner) av_addf(&a, "--attach", xid);
        if (*title) av_addf(&a, "--title", title);
        vd_filters(&a, kind == VD_FOLDER ? NULL : filters, kd, sizeof kd);
        if (kind == VD_FOLDER) av_add(&a, "--getexistingdirectory");
        else if (kind == VD_SAVE) av_add(&a, "--getsavefilename");
        else av_add(&a, "--getopenfilename");
        if (kind == VD_OPEN_MANY) { av_add(&a, "--multiple"); av_add(&a, "--separate-output"); }
        /* kdialog's start dir and filter are positional, in that order */
        if (*start || kd[0]) av_add_path(&a, *start ? start : ".");
        if (kd[0] && kind != VD_FOLDER) av_add(&a, kd);
    }
    char *o = NULL;
    size_t ol = 0;
    int rc = vd_run(a.v, &o, &ol);
    av_free(&a);
    /* 1 is Cancel (both programs), 5 zenity's timeout. Anything else -- 127
       from a loader that could not start it, a crash -- is not the person
       saying no, so it must not look like a cancelled dialog. */
    VdResult *r = vd_result(rc == 0 ? VD_OK : (rc == 1 || rc == 5) ? VD_CANCELLED : VD_UNAVAILABLE);
    if (r && rc == 0 && o) {
        /* one path per line; the last newline is the program's own */
        size_t i = 0;
        while (i < ol) {
            size_t j = i;
            while (j < ol && o[j] != '\n') j++;
            if (j > i) vd_push(r, o + i, j - i);
            i = j + 1;
        }
        if (r->n == 0) r->status = VD_CANCELLED;
    }
    free(o);
    return r;
}

int32_t vdesk_message(int32_t kind, const char *title, const char *text, unsigned long owner) {
    vd_pick_backend();
    if (g_backend == BK_NONE) return -1;
    if (!title) title = "";
    if (!text) text = "";
    Argv a = { {0}, 0 };
    av_add(&a, g_backend_path);
    char xid[32];
    snprintf(xid, sizeof xid, "%lu", owner);
    if (g_backend == BK_ZENITY) {
        static const char *z[] = { "--info", "--warning", "--error", "--question" };
        av_add(&a, z[kind >= 0 && kind <= 3 ? kind : 0]);
        if (*title) av_addf(&a, "--title", title);
        av_addf(&a, "--text", text);
        av_add(&a, "--no-markup");   /* the text is text, not Pango markup */
        if (owner && g_zenity_major < 4) av_addf(&a, "--attach", xid);
    } else {
        static const char *k[] = { "--msgbox", "--sorry", "--error", "--yesno" };
        if (owner) av_addf(&a, "--attach", xid);
        if (*title) av_addf(&a, "--title", title);
        av_addf(&a, k[kind >= 0 && kind <= 3 ? kind : 0], text);
    }
    char *o = NULL;
    size_t ol = 0;
    int rc = vd_run(a.v, &o, &ol);
    av_free(&a);
    free(o);
    if (rc == 0) return 1;
    if (rc == 1 || rc == 5) return 0;   /* No / Cancel / timeout, as above */
    return -1;
}

int32_t vdesk_open(const char *target) {
    if (!target || !*target) return 0;
    char p[4096];
    const char *prog = getenv("VYTO_DESKTOP_OPEN");   /* tests; else xdg-open */
    if (!prog || !*prog) prog = "xdg-open";
    if (!vd_which(prog, p, sizeof p)) return 0;
    Argv a = { {0}, 0 };
    av_add(&a, p);
    av_add_path(&a, target);   /* "-x" would be an option to xdg-open */
    int ok = vd_spawn_detached(a.v);
    av_free(&a);
    return ok;
}

const char *vdesk_dir(int32_t which, const char *app) {
    static const char *envs[] = { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME" };
    static const char *defs[] = { ".config", ".local/share", ".cache" };
    if (which < 0 || which > 2) return "";
    char tmp[4096];
    const char *x = getenv(envs[which]);
    int n;
    /* The XDG spec: a relative value is invalid and must be ignored. */
    if (x && x[0] == '/') {
        n = snprintf(tmp, sizeof tmp, "%s/%s", x, app);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : NULL;
        }
        if (!home || !*home) return "";
        n = snprintf(tmp, sizeof tmp, "%s/%s/%s", home, defs[which], app);
    }
    if (n < 0 || (size_t)n >= sizeof tmp) return "";
    return vd_dir_out(tmp);
}

/* ======================================================================== */
#else /* freestanding or Android: nothing to talk to */

int32_t vdesk_available(void) { return 0; }
void *vdesk_dialog(int32_t kind, const char *title, const char *start, const char *filters,
                   unsigned long owner) {
    (void)kind; (void)title; (void)start; (void)filters; (void)owner;
    return (void *)0;
}
int32_t vdesk_result_status(void *h) { (void)h; return VD_UNAVAILABLE; }
int32_t vdesk_result_count(void *h) { (void)h; return 0; }
const char *vdesk_result_path(void *h, int32_t i) { (void)h; (void)i; return ""; }
void vdesk_result_free(void *h) { (void)h; }
int32_t vdesk_message(int32_t kind, const char *title, const char *text, unsigned long owner) {
    (void)kind; (void)title; (void)text; (void)owner;
    return -1;
}
int32_t vdesk_open(const char *target) { (void)target; return 0; }
const char *vdesk_dir(int32_t which, const char *app) { (void)which; (void)app; return ""; }

#endif
