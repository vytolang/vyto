/* vyto/desktop/notify native backing — the OS's own notifications.

   Linux/BSD: the freedesktop Notifications service on the session bus
   (org.freedesktop.Notifications.Notify), spoken directly through libdbus,
   which is dlopen'ed. Not notify-send: a process per notice is slow from a big
   process, needs libnotify-bin, and throws away the id the server returns —
   and that id is what lets a notice be updated in place or closed. Not gdbus:
   its arguments are GVariant text, which turns every title into a quoting
   problem. Typed D-Bus arguments have no quoting at all.

   libdbus is on every systemd desktop, and dlopen keeps it off the link line,
   so a program that imports this package still starts on a box without it and
   reports notify_available() false.

   THE CALLER NEVER WAITS FOR THE SERVER. GNOME 42 shows a banner at once but
   answers Notify only 2.5-6 s later (measured 2026-11-19; notify-send stalls
   the same way), so a blocking call would freeze an app's UI for seconds per
   notice. The caller's thread only checks availability once (two calls that
   *are* answered in under a millisecond) and then queues jobs. One C worker
   thread sends them in order without waiting, and collects the replies as they
   come. It waits for a reply only when a later replace or close needs that
   notice's server id, so a burst of notices goes out together, not one GNOME
   reply apart. Jobs hold copies of their strings: no Vyto object ever reaches
   that thread.

   Ids handed to Vyto are therefore local, counting up from 1, and mapped to the
   server's ids by the worker. At exit, the process waits (bounded, 2 s) until
   every queued notice has been *written* to the bus, not answered, so a tool
   that notifies and exits at once still gets its notice shown.

   Windows: one tray icon per process (Shell_NotifyIconW, a hidden owner window)
   whose balloon carries the notice; Windows 10 and later show that balloon as a
   toast. The icon is the exe's own (resource 1, which vytoc --icon writes),
   else the stock application icon. It is removed at exit so no ghost icon is
   left in the tray. The shell answers at once, so there is no worker.

   VYTO_NOTIFY=off makes every call answer "unavailable", which is how the
   no-notifications path is tested.

   Every entry point has a stub arm (freestanding, Android, macOS) answering
   "unavailable": 0 ids, 0 results. */

#include <stdint.h>

#if defined(VT_NO_LIBC) || defined(__ANDROID__) || defined(__APPLE__)

int32_t vnot_available(void) { return 0; }
void vnot_set_app(const char *name) { (void)name; }
void vnot_set_quiet(int32_t on) { (void)on; }
int32_t vnot_show(int32_t replaces, const char *title, const char *body, int32_t kind, int32_t timeout_ms) {
    (void)replaces; (void)title; (void)body; (void)kind; (void)timeout_ms;
    return 0;
}
int32_t vnot_close(int32_t id) { (void)id; return 0; }
const char *vnot_server(void) { return ""; }

#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char g_app[128] = "Vyto";
static int g_quiet;

static int notify_off(void) {
    const char *e = getenv("VYTO_NOTIFY");
    return e && strcmp(e, "off") == 0;
}

void vnot_set_app(const char *name) {
    if (name && *name) snprintf(g_app, sizeof g_app, "%s", name);
}

void vnot_set_quiet(int32_t on) { g_quiet = on != 0; }

/* ======================================================================== */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

static HWND g_owner;
static int g_icon_added;
static int32_t g_next_id;    /* ids handed out so far */
static int32_t g_showing;    /* the id whose balloon is up, 0 = none */

static void to_wide(const char *s, wchar_t *out, int cap) {
    out[0] = 0;
    if (!s || !*s) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) != 0) return;
    /* Too long for the field: the shell truncates nothing itself, so convert
       the whole string and cut it to fit. */
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *tmp = need > 0 ? malloc(sizeof(wchar_t) * (size_t)need) : NULL;
    if (!tmp) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp, need);
    memcpy(out, tmp, sizeof(wchar_t) * (size_t)(cap - 1));
    out[cap - 1] = 0;
    /* never end on a lone high surrogate */
    if (out[cap - 2] >= 0xD800 && out[cap - 2] <= 0xDBFF) out[cap - 2] = 0;
    free(tmp);
}

static void nid_init(NOTIFYICONDATAW *nid) {
    memset(nid, 0, sizeof *nid);
    nid->cbSize = sizeof *nid;
    nid->hWnd = g_owner;
    nid->uID = 1;
}

static void remove_icon(void) {
    if (!g_icon_added) return;
    NOTIFYICONDATAW nid;
    nid_init(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_icon_added = 0;
}

static int ensure_icon(void) {
    if (g_icon_added) return 1;
    if (!g_owner) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"VytoNotifyOwner";
        RegisterClassW(&wc);   /* fails harmlessly if already registered */
        /* A hidden top-level window rather than HWND_MESSAGE: the shell sends
           tray messages to it, and message-only windows miss broadcasts such
           as TaskbarCreated. It is never shown. */
        g_owner = CreateWindowExW(0, L"VytoNotifyOwner", L"", WS_OVERLAPPED, 0, 0, 0, 0,
                                  NULL, NULL, wc.hInstance, NULL);
        if (!g_owner) return 0;
    }
    NOTIFYICONDATAW nid;
    nid_init(&nid);
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    to_wide(g_app, nid.szTip, (int)(sizeof nid.szTip / sizeof nid.szTip[0]));
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return 0;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    g_icon_added = 1;
    atexit(remove_icon);
    return 1;
}

int32_t vnot_available(void) {
    if (notify_off()) return 0;
    return ensure_icon();
}

const char *vnot_server(void) { return vnot_available() ? "Shell_NotifyIcon" : ""; }

/* One icon, one balloon: any notice replaces the one showing. A replace keeps
   its id, as on Linux; close(id) of a notice that something else has since
   replaced does nothing. */
int32_t vnot_show(int32_t replaces, const char *title, const char *body, int32_t kind, int32_t timeout_ms) {
    if (notify_off() || !ensure_icon()) return 0;
    NOTIFYICONDATAW nid;
    nid_init(&nid);
    nid.uFlags = NIF_INFO;
    to_wide(title, nid.szInfoTitle, (int)(sizeof nid.szInfoTitle / sizeof nid.szInfoTitle[0]));
    /* an empty szInfo means "take the balloon down", so a title-only notice
       carries one space */
    to_wide(body && *body ? body : " ", nid.szInfo, (int)(sizeof nid.szInfo / sizeof nid.szInfo[0]));
    nid.dwInfoFlags = kind == 2 ? NIIF_ERROR : kind == 1 ? NIIF_WARNING : NIIF_INFO;
    if (g_quiet) nid.dwInfoFlags |= NIIF_NOSOUND;
    /* Ignored from Vista on (the system decides), honoured by XP. */
    nid.uTimeout = timeout_ms > 0 ? (UINT)timeout_ms : 10000;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) return 0;
    g_showing = (replaces > 0 && replaces <= g_next_id) ? replaces : ++g_next_id;
    return g_showing;
}

int32_t vnot_close(int32_t id) {
    if (!g_icon_added || id <= 0 || id != g_showing) return 0;
    NOTIFYICONDATAW nid;
    nid_init(&nid);
    nid.uFlags = NIF_INFO;   /* an empty szInfo takes the balloon down */
    g_showing = 0;
    return Shell_NotifyIconW(NIM_MODIFY, &nid) ? 1 : 0;
}

/* ======================================================================== */
#else  /* Linux and the BSDs: freedesktop Notifications over libdbus */

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* The libdbus ABI, declared here rather than included so that building needs
   no dbus headers. Every type below is part of libdbus's stable public ABI
   (libdbus-1.so.3). The two structs the caller allocates are given more room
   than the real ones need: libdbus only ever writes inside its own layout. */
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef struct DBusPendingCall DBusPendingCall;
typedef uint32_t dbus_bool_t;
typedef struct {
    const char *name;
    const char *message;
    unsigned int dummy;
    void *padding1;
    void *spare[4];
} DBusError;
typedef union { void *p[16]; int64_t l[8]; } DBusMessageIter;

#define DBUS_BUS_SESSION 0
#define T_STRING  ((int)'s')
#define T_UINT32  ((int)'u')
#define T_INT32   ((int)'i')
#define T_BYTE    ((int)'y')
#define T_BOOLEAN ((int)'b')
#define T_ARRAY   ((int)'a')
#define T_VARIANT ((int)'v')
#define T_DICT    ((int)'e')

static struct {
    dbus_bool_t (*threads_init)(void);
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    DBusConnection *(*bus_get)(int, DBusError *);
    void (*set_exit_on_disconnect)(DBusConnection *, dbus_bool_t);
    DBusMessage *(*new_call)(const char *, const char *, const char *, const char *);
    void (*iter_init_append)(DBusMessage *, DBusMessageIter *);
    dbus_bool_t (*append_basic)(DBusMessageIter *, int, const void *);
    dbus_bool_t (*open_container)(DBusMessageIter *, int, const char *, DBusMessageIter *);
    dbus_bool_t (*close_container)(DBusMessageIter *, DBusMessageIter *);
    DBusMessage *(*send_block)(DBusConnection *, DBusMessage *, int, DBusError *);
    dbus_bool_t (*send_reply)(DBusConnection *, DBusMessage *, DBusPendingCall **, int);
    dbus_bool_t (*send)(DBusConnection *, DBusMessage *, uint32_t *);
    void (*flush)(DBusConnection *);
    void (*pending_block)(DBusPendingCall *);
    dbus_bool_t (*pending_done)(DBusPendingCall *);
    dbus_bool_t (*read_write_dispatch)(DBusConnection *, int);
    DBusMessage *(*pending_steal)(DBusPendingCall *);
    void (*pending_unref)(DBusPendingCall *);
    dbus_bool_t (*iter_init)(DBusMessage *, DBusMessageIter *);
    int (*iter_type)(DBusMessageIter *);
    void (*iter_get)(DBusMessageIter *, void *);
    dbus_bool_t (*iter_next)(DBusMessageIter *);
    void (*iter_recurse)(DBusMessageIter *, DBusMessageIter *);
    void (*unref)(DBusMessage *);
} D;

static DBusConnection *g_bus;
static int g_state;          /* 0 not tried, 1 up, -1 unavailable */
static int g_markup;         /* the server reads body as markup: escape it */
static char g_server[128];

/* The two availability calls run on the caller's thread and every server
   answers them at once; this bounds a wedged one. The worker's Notify gets
   far longer, since nothing waits on it. */
#define PROBE_MS 1000
#define NOTIFY_MS 15000
/* How long exit waits for queued notices to be written out. */
#define EXIT_WAIT_MS 2000
/* A burst beyond this many unsent notices is dropped rather than queued. */
#define MAX_QUEUED 64

static int load(void) {
    void *h = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
#define SYM(field, name) if (!(*(void **)&D.field = dlsym(h, name))) return 0
    SYM(threads_init, "dbus_threads_init_default");
    SYM(error_init, "dbus_error_init");
    SYM(error_free, "dbus_error_free");
    SYM(bus_get, "dbus_bus_get");
    SYM(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
    SYM(new_call, "dbus_message_new_method_call");
    SYM(iter_init_append, "dbus_message_iter_init_append");
    SYM(append_basic, "dbus_message_iter_append_basic");
    SYM(open_container, "dbus_message_iter_open_container");
    SYM(close_container, "dbus_message_iter_close_container");
    SYM(send_block, "dbus_connection_send_with_reply_and_block");
    SYM(send_reply, "dbus_connection_send_with_reply");
    SYM(send, "dbus_connection_send");
    SYM(flush, "dbus_connection_flush");
    SYM(pending_block, "dbus_pending_call_block");
    SYM(pending_done, "dbus_pending_call_get_completed");
    SYM(read_write_dispatch, "dbus_connection_read_write_dispatch");
    SYM(pending_steal, "dbus_pending_call_steal_reply");
    SYM(pending_unref, "dbus_pending_call_unref");
    SYM(iter_init, "dbus_message_iter_init");
    SYM(iter_type, "dbus_message_iter_get_arg_type");
    SYM(iter_get, "dbus_message_iter_get_basic");
    SYM(iter_next, "dbus_message_iter_next");
    SYM(iter_recurse, "dbus_message_iter_recurse");
    SYM(unref, "dbus_message_unref");
#undef SYM
    return 1;   /* the handle stays open for the life of the process */
}

static DBusMessage *call(const char *method) {
    return D.new_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                      "org.freedesktop.Notifications", method);
}

static DBusMessage *probe(const char *method) {
    DBusMessage *m = call(method);
    if (!m) return NULL;
    DBusError err;
    D.error_init(&err);
    DBusMessage *r = D.send_block(g_bus, m, PROBE_MS, &err);
    D.unref(m);
    if (err.name) D.error_free(&err);
    return r;
}

/* ---- the job queue and the worker ------------------------------------ */

enum { JOB_SHOW, JOB_CLOSE };

typedef struct Job {
    struct Job *next;
    int op;
    int32_t local;           /* the id Vyto holds */
    char *app, *sum, *bod;   /* owned copies, already made valid UTF-8 */
    int kind, timeout_ms, quiet;
} Job;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;   /* a job arrived */
static pthread_cond_t g_sent = PTHREAD_COND_INITIALIZER;   /* g_unsent fell */
static Job *g_head, *g_tail;
static int g_unsent;         /* queued or taken but not yet written to the bus */
static int32_t g_next_local;

/* local id -> server id, for the most recent notices. A replace or close of
   an id older than this ring falls back to a new notice / a no-op. */
#define MAP_N 128
static struct { int32_t local; uint32_t server; } g_map[MAP_N];

static uint32_t map_get(int32_t local) {
    for (int i = 0; i < MAP_N; i++) if (g_map[i].local == local) return g_map[i].server;
    return 0;
}

static void map_set(int32_t local, uint32_t server) {
    g_map[local % MAP_N].local = local;
    g_map[local % MAP_N].server = server;
}

static void job_free(Job *j) {
    free(j->app);
    free(j->sum);
    free(j->bod);
    free(j);
}

static void hint(DBusMessageIter *dict, const char *key, int type, const void *val) {
    char sig[2] = { (char)type, 0 };
    DBusMessageIter e, v;
    D.open_container(dict, T_DICT, NULL, &e);
    D.append_basic(&e, T_STRING, &key);
    D.open_container(&e, T_VARIANT, sig, &v);
    D.append_basic(&v, type, val);
    D.close_container(&e, &v);
    D.close_container(dict, &e);
}

/* Notify(s app, u replaces, s icon, s summary, s body, as actions,
          a{sv} hints, i timeout) -> u id */
static DBusMessage *build_notify(Job *j, uint32_t replaces) {
    DBusMessage *m = call("Notify");
    if (!m) return NULL;
    const char *icon = j->kind == 2 ? "dialog-error" : j->kind == 1 ? "dialog-warning" : "dialog-information";
    int32_t tmo = j->timeout_ms > 0 ? j->timeout_ms : -1;   /* -1: the server's default */
    unsigned char urgency = j->kind == 2 ? 2 : 1;           /* critical, else normal */
    dbus_bool_t quiet = 1;
    DBusMessageIter it, arr, dict;
    D.iter_init_append(m, &it);
    D.append_basic(&it, T_STRING, &j->app);
    D.append_basic(&it, T_UINT32, &replaces);
    D.append_basic(&it, T_STRING, &icon);
    D.append_basic(&it, T_STRING, &j->sum);
    D.append_basic(&it, T_STRING, &j->bod);
    D.open_container(&it, T_ARRAY, "s", &arr);
    D.close_container(&it, &arr);
    D.open_container(&it, T_ARRAY, "{sv}", &dict);
    hint(&dict, "urgency", T_BYTE, &urgency);
    if (j->quiet) hint(&dict, "suppress-sound", T_BOOLEAN, &quiet);
    D.close_container(&it, &dict);
    D.append_basic(&it, T_INT32, &tmo);
    return m;
}

/* Notify calls sent and not yet answered. Only the worker touches these. */
static struct { int32_t local; DBusPendingCall *pc; } g_wait[MAX_QUEUED];
static int g_nwait;

/* Take a finished call's reply: its server id goes in the map. */
static void settle(int i) {
    DBusMessage *r = D.pending_steal(g_wait[i].pc);
    D.pending_unref(g_wait[i].pc);
    uint32_t id = 0;
    if (r) {
        DBusMessageIter ri;
        if (D.iter_init(r, &ri) && D.iter_type(&ri) == T_UINT32) D.iter_get(&ri, &id);
        D.unref(r);
    }
    pthread_mutex_lock(&g_mu);
    map_set(g_wait[i].local, id);
    pthread_mutex_unlock(&g_mu);
    g_wait[i] = g_wait[--g_nwait];
}

static void settle_finished(void) {
    for (int i = g_nwait - 1; i >= 0; i--)
        if (D.pending_done(g_wait[i].pc)) settle(i);
}

/* The server id of a notice this process showed, waiting for its reply if
   it is still outstanding. Every outstanding call for that id is settled, in
   the order sent, so the map ends on the newest answer. */
static uint32_t server_id(int32_t local) {
    for (int i = 0; i < g_nwait;) {
        if (g_wait[i].local == local) {
            D.pending_block(g_wait[i].pc);
            settle(i);
            i = 0;
        } else {
            i++;
        }
    }
    pthread_mutex_lock(&g_mu);
    uint32_t id = map_get(local);
    pthread_mutex_unlock(&g_mu);
    return id;
}

static void mark_sent(void) {
    pthread_mutex_lock(&g_mu);
    g_unsent--;
    pthread_cond_broadcast(&g_sent);
    pthread_mutex_unlock(&g_mu);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_head && g_nwait == 0) pthread_cond_wait(&g_work, &g_mu);
        Job *j = g_head;
        if (j) {
            g_head = j->next;
            if (!g_head) g_tail = NULL;
        }
        pthread_mutex_unlock(&g_mu);

        if (!j) {
            /* Nothing to send, replies outstanding: read for a while, then
               look at the queue again. A disconnected bus answers nothing
               more, so give up on what it owes. */
            if (!D.read_write_dispatch(g_bus, 50)) {
                while (g_nwait > 0) {
                    D.pending_unref(g_wait[g_nwait - 1].pc);
                    g_nwait--;
                }
            }
            settle_finished();
            continue;
        }

        if (j->op == JOB_CLOSE) {
            uint32_t server = server_id(j->local);
            if (server) {
                DBusMessage *m = call("CloseNotification");
                if (m) {
                    DBusMessageIter it;
                    D.iter_init_append(m, &it);
                    D.append_basic(&it, T_UINT32, &server);
                    D.send(g_bus, m, NULL);
                    D.unref(m);
                    D.flush(g_bus);
                }
            }
            mark_sent();
            job_free(j);
            continue;
        }

        /* A replace needs the server id of the notice it replaces; a new
           notice (an id not yet in the map, nothing outstanding) gets 0. */
        uint32_t server = server_id(j->local);
        DBusMessage *m = build_notify(j, server);
        DBusPendingCall *pc = NULL;
        if (m) {
            D.send_reply(g_bus, m, &pc, NOTIFY_MS);
            D.unref(m);
            D.flush(g_bus);
        }
        mark_sent();
        if (pc && g_nwait < MAX_QUEUED) {
            g_wait[g_nwait].local = j->local;
            g_wait[g_nwait].pc = pc;
            g_nwait++;
        } else if (pc) {
            D.pending_unref(pc);   /* table full: the id is forgotten */
        }
        settle_finished();
        job_free(j);
    }
    return NULL;
}

/* At exit: let queued notices reach the bus. Bounded, because a wedged
   session bus must not keep a closing app alive. */
static void drain_at_exit(void) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += EXIT_WAIT_MS / 1000;
    until.tv_nsec += (long)(EXIT_WAIT_MS % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    while (g_unsent > 0)
        if (pthread_cond_timedwait(&g_sent, &g_mu, &until) == ETIMEDOUT) break;
    pthread_mutex_unlock(&g_mu);
}

static int up(void) {
    if (notify_off()) return 0;
    if (g_state != 0) return g_state > 0;
    g_state = -1;
    /* Without either, libdbus would try to autolaunch a bus over X11: a new
       daemon nobody asked for. No session, no notices. */
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") && !getenv("XDG_RUNTIME_DIR")) return 0;
    if (!load()) return 0;
    /* The worker and the caller's thread both touch the connection. */
    if (!D.threads_init()) return 0;
    DBusError err;
    D.error_init(&err);
    g_bus = D.bus_get(DBUS_BUS_SESSION, &err);
    if (err.name) D.error_free(&err);
    if (!g_bus) return 0;
    /* libdbus's shared session connection calls _exit() when the bus goes
       away unless told otherwise. A notification library must never be the
       reason an app dies. */
    D.set_exit_on_disconnect(g_bus, 0);

    /* Who is serving, and does it read markup? Asking is also the
       availability check: no server, no notices. */
    DBusMessage *r = probe("GetServerInformation");
    if (!r) return 0;
    DBusMessageIter it;
    if (D.iter_init(r, &it) && D.iter_type(&it) == T_STRING) {
        const char *s = NULL;
        D.iter_get(&it, &s);
        if (s) snprintf(g_server, sizeof g_server, "%s", s);
    }
    D.unref(r);
    r = probe("GetCapabilities");
    if (r) {
        DBusMessageIter a, e;
        if (D.iter_init(r, &a) && D.iter_type(&a) == T_ARRAY) {
            D.iter_recurse(&a, &e);
            while (D.iter_type(&e) == T_STRING) {
                const char *s = NULL;
                D.iter_get(&e, &s);
                if (s && strcmp(s, "body-markup") == 0) g_markup = 1;
                if (!D.iter_next(&e)) break;
            }
        }
        D.unref(r);
    }

    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int ok = pthread_create(&t, &at, worker, NULL) == 0;
    pthread_attr_destroy(&at);
    if (!ok) return 0;
    atexit(drain_at_exit);
    g_state = 1;
    return 1;
}

int32_t vnot_available(void) { return up(); }
const char *vnot_server(void) { return up() ? g_server : ""; }

/* libdbus treats invalid UTF-8 in a string argument as a programming error and
   aborts the process by default. Vyto strings are UTF-8 by convention, not by
   proof (a file read off disk is bytes), so every string is made valid here:
   a malformed sequence becomes U+FFFD. With markup, & < > are escaped too, or
   "Tea & milk" would arrive as broken markup and show nothing. Malloc'd. */
static char *clean(const char *s, int markup) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *o = malloc(n * 5 + 1);   /* worst case: every byte becomes "&amp;" */
    if (!o) return NULL;
    size_t j = 0;
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        int ok = len > 0 && i + (size_t)len <= n;
        for (int k = 1; ok && k < len; k++) if ((p[i + k] & 0xC0) != 0x80) ok = 0;
        if (ok && len > 1) {
            /* overlong forms, surrogates and > U+10FFFF are invalid too */
            uint32_t cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k = 1; k < len; k++) cp = (cp << 6) | (p[i + k] & 0x3F);
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) ok = 0;
        }
        if (!ok) {
            memcpy(o + j, "\xEF\xBF\xBD", 3);
            j += 3;
            i += 1;
            continue;
        }
        if (markup && len == 1 && (c == '&' || c == '<' || c == '>')) {
            const char *rep = c == '&' ? "&amp;" : c == '<' ? "&lt;" : "&gt;";
            size_t rl = strlen(rep);
            memcpy(o + j, rep, rl);
            j += rl;
        } else {
            memcpy(o + j, p + i, (size_t)len);
            j += (size_t)len;
        }
        i += (size_t)len;
    }
    o[j] = 0;
    return o;
}

static int enqueue(Job *j) {
    pthread_mutex_lock(&g_mu);
    if (g_unsent >= MAX_QUEUED) {
        pthread_mutex_unlock(&g_mu);
        job_free(j);
        return 0;
    }
    if (j->op == JOB_SHOW && j->local == 0) j->local = ++g_next_local;
    int32_t id = j->local;
    if (g_tail) g_tail->next = j; else g_head = j;
    g_tail = j;
    g_unsent++;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_mu);
    return id;
}

int32_t vnot_show(int32_t replaces, const char *title, const char *body, int32_t kind, int32_t timeout_ms) {
    if (!up()) return 0;
    Job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->op = JOB_SHOW;
    j->app = clean(g_app, 0);
    j->sum = clean(title, 0);       /* the summary is never markup */
    j->bod = clean(body, g_markup);
    j->kind = kind;
    j->timeout_ms = timeout_ms;
    j->quiet = g_quiet;
    if (!j->app || !j->sum || !j->bod) { job_free(j); return 0; }
    /* A replace keeps the id it was given; one this process never handed out
       is a new notice. */
    pthread_mutex_lock(&g_mu);
    if (replaces > 0 && replaces <= g_next_local) j->local = replaces;
    pthread_mutex_unlock(&g_mu);
    return enqueue(j);
}

/* True when the close was queued for a notice this process showed. Whether it
   was still on screen is the server's business, and it does not say. */
int32_t vnot_close(int32_t id) {
    if (!up()) return 0;
    pthread_mutex_lock(&g_mu);
    int known = id > 0 && id <= g_next_local;
    pthread_mutex_unlock(&g_mu);
    if (!known) return 0;
    Job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->op = JOB_CLOSE;
    j->local = id;
    return enqueue(j) != 0;
}

#endif /* _WIN32 */
#endif /* stub arm */
