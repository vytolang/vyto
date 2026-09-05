/* vsurf_android.c — the Android backend for the vsurf surface shim.
 *
 * A separate file rather than a fourth #ifdef arm in vsurf.c, because it
 * shares nothing with the others. The X11 and fbdev arms are *drawing*
 * backends built around a backbuffer; this one owns no pixels at all. Vyto
 * renders through AndroidPainter into a Java Bitmap, so all this file does is
 * bridge events from the UI thread and answer the surface's metadata queries.
 *
 * STATUS: STUB — never compiled.
 *
 * The companion vsurf.c patch IS in place: an `#ifndef __ANDROID__` wraps its
 * whole body (vsurf.c:35, closed at :2201), so under `-D__ANDROID__` the X11
 * arm compiles to nothing and this file owns the backend. What is still
 * missing is an NDK toolchain to compile either of them with.
 *
 * Design of record: local/docs/ANDROID.md.
 */
#ifdef __ANDROID__

#include "vsurf.h"

/* Reaches across packages, because this file and the android shims are a
 * mutually-dependent pair: jni_boot.c calls the vs_android_* functions defined
 * below, and this file calls vta_logf/vta_view defined there. Everything links
 * into one .so, so the only question is whether the prototypes get checked —
 * and for eight functions crossing a thread boundary, they should be. vytoc
 * only adds -I for the package being compiled, so the path is relative to this
 * file rather than an -I. Moving either directory breaks this include loudly,
 * which is the right failure. */
#include "../../../mobile/android/native/src/vyto_android.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* android.view.MotionEvent constants, so no translation table is needed on
 * either side of the JNI boundary. */
#define AMOTION_DOWN          0
#define AMOTION_UP            1
#define AMOTION_MOVE          2
#define AMOTION_CANCEL        3
#define AMOTION_POINTER_DOWN  5
#define AMOTION_POINTER_UP    6

/* ---------------------------------------------------------------- events */

typedef struct {
    int   type;      /* VS_EV_* */
    int   key;
    char *text;      /* owned, or NULL */
    int   mods;
    int   x, y;
    int   wheel;
} Ev;

#define QCAP 256

static struct {
    int      bound;
    int      w, h;
    float    density;

    /* insets, in px. Stored but NOT delivered: the event enum is closed at 11
     * with no VS_EV_INSETS, so Vyto cannot currently see these. In practice
     * the IME inset arrives as a RESIZE anyway because the manifest sets
     * adjustResize, which is why this is survivable — but safe-area layout
     * (notch, nav bar) needs a real event. Track B. */
    int      ins_l, ins_t, ins_r, ins_b, ins_ime;

    Ev       q[QCAP];
    int      head, tail, count;

    int      paused;
    int      closing;

    pthread_mutex_t lock;
    pthread_cond_t  cv;
} S;

/* Per-event state, matching vsurf's model: the accessors describe whatever
 * event was last *delivered*, not what is queued. Only the Vyto thread reads
 * these, and only from inside a deliver, so they need no lock.
 *
 * These stay file-scope rather than moving into a per-surface struct the way
 * the desktop arms did: Android is single-window by construction — the whole
 * platform binding is one static `S`, and vs_open attaches to it rather than
 * allocating. The accessors still take a surface handle so the ABI matches the
 * other arms; they just have nothing to look it up in. */
static int   last_key, last_x, last_y, last_wheel, last_mods;

/* Deliberately heap, not the 32-byte fixed buffer vsurf.c uses (vsurf.c:60).
 * Android IME commits — autocomplete, paste, emoji sequences — routinely
 * exceed 32 bytes and would be silently dropped by that guard. */
static char *last_text_buf = NULL;
static size_t last_text_cap = 0;

static void set_last_text(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n + 1 > last_text_cap) {
        size_t cap = last_text_cap ? last_text_cap : 32;
        while (cap < n + 1) cap *= 2;
        char *nb = (char *)realloc(last_text_buf, cap);
        if (!nb) return;
        last_text_buf = nb;
        last_text_cap = cap;
    }
    if (last_text_buf) memcpy(last_text_buf, s ? s : "", n + 1);
}

/* Caller holds the lock. Drops the oldest on overflow: a stale queue is worse
 * than a dropped event, and the only way to overflow is the UI thread
 * outrunning a stalled Vyto thread, in which case the old events are moot. */
static void push_locked(const Ev *e) {
    if (S.count == QCAP) {
        free(S.q[S.head].text);
        S.head = (S.head + 1) % QCAP;
        S.count--;
    }
    S.q[S.tail] = *e;
    S.tail = (S.tail + 1) % QCAP;
    S.count++;
    pthread_cond_signal(&S.cv);
}

static void push(const Ev *e) {
    pthread_mutex_lock(&S.lock);
    push_locked(e);
    pthread_mutex_unlock(&S.lock);
}

/* Caller holds the lock. Copies out and transfers ownership of text. */
static int pop_locked(Ev *out) {
    if (S.count == 0) return 0;
    *out = S.q[S.head];
    S.q[S.head].text = NULL;
    S.head = (S.head + 1) % QCAP;
    S.count--;
    return 1;
}

/* Publish an event's payload into the accessor state, then free its text. */
static int deliver(Ev *e) {
    last_key   = e->key;
    last_mods  = e->mods;
    last_x     = e->x;
    last_y     = e->y;
    last_wheel = e->wheel;
    set_last_text(e->text ? e->text : "");
    free(e->text);
    e->text = NULL;
    return e->type;
}

/* ------------------------------------------------------------ UI thread in */

void vs_android_bind(int w, int h, float density) {
    /* Insets outlive a bind. onApplyWindowInsets fires during the first
     * traversal, which is *before* Native.start on every launch measured here,
     * and Android then does not repeat it — so a plain memset delivers them
     * once and throws them away. They describe the display, not the binding
     * session. Zero on the first call, since S is a zeroed static. */
    int il = S.ins_l, it = S.ins_t, ir = S.ins_r, ib = S.ins_b, iime = S.ins_ime;
    memset(&S, 0, sizeof S);
    S.ins_l = il; S.ins_t = it; S.ins_r = ir; S.ins_b = ib; S.ins_ime = iime;
    pthread_mutex_init(&S.lock, NULL);
    pthread_cond_init(&S.cv, NULL);
    S.w = w;
    S.h = h;
    S.density = density > 0.0f ? density : 1.0f;
    S.bound = 1;

    /* The first thing the loop should do is paint. */
    Ev e = {0};
    e.type = VS_EV_EXPOSE;
    push(&e);
}

void vs_android_unbind(void) {
    pthread_mutex_lock(&S.lock);
    while (S.count > 0) {
        free(S.q[S.head].text);
        S.head = (S.head + 1) % QCAP;
        S.count--;
    }
    S.bound = 0;
    pthread_cond_broadcast(&S.cv);
    pthread_mutex_unlock(&S.lock);

    free(last_text_buf);
    last_text_buf = NULL;
    last_text_cap = 0;
}

void vs_android_push_touch(int action, int pointer_id, float x, float y,
                           int64_t time_ms) {
    (void)time_ms;
    /* Single pointer only. The event model cannot express a second one: an
     * event is an int plus six argument-less globals (vs_key/vs_x/vs_y/...),
     * so there is nowhere to put a pointer id. Multitouch needs the queued
     * per-surface event struct that PORTABILITY.md:251-256 already scopes to
     * the multiwindow work. */
    if (pointer_id != 0) return;

    Ev e = {0};
    e.x = (int)x;
    e.y = (int)y;

    switch (action) {
        case AMOTION_DOWN:
        case AMOTION_POINTER_DOWN:
            e.type = VS_EV_MOUSE_DOWN;
            break;
        case AMOTION_UP:
        case AMOTION_POINTER_UP:
        case AMOTION_CANCEL:
            /* A cancel is delivered as an up so widgets release their press
             * state. Losing the distinction is wrong in principle — a cancel
             * should not fire on_click — but Vyto has no cancel concept and a
             * stuck-pressed widget is the worse failure. */
            e.type = VS_EV_MOUSE_UP;
            break;
        case AMOTION_MOVE:
            e.type = VS_EV_MOUSE_MOVE;
            break;
        default:
            return;
    }

    pthread_mutex_lock(&S.lock);
    /* Coalesce consecutive moves, same as the X11 arm does with MotionNotify
     * (vsurf.c:2098-2108). Each delivered move drives a hit-test and a repaint,
     * so replaying a batch of historical samples would cost N layout passes for
     * one frame of finger travel.
     *
     * Drag-scroll and fling now exist (Track C item 2, core.vt drag_scroll),
     * and coalescing stays correct for them: velocity there is measured as
     * distance over elapsed time between delivered moves, and collapsing two
     * moves into one preserves both. It would only break if velocity were
     * accumulated per-event instead. */
    if (e.type == VS_EV_MOUSE_MOVE && S.count > 0) {
        int last = (S.tail - 1 + QCAP) % QCAP;
        if (S.q[last].type == VS_EV_MOUSE_MOVE) {
            S.q[last].x = e.x;
            S.q[last].y = e.y;
            pthread_mutex_unlock(&S.lock);
            return;
        }
    }
    push_locked(&e);

    /* A finger that lifts is GONE — there is no cursor left hovering where it
     * was. The toolkit's hover state is maintained by mouse-move (core.vt's
     * set_hover), and on_mouse_up deliberately clears the press but not the
     * hover, because on a desktop the pointer really is still there. With no
     * pointer-leave equivalent on touch, the last-tapped widget stays lit
     * forever and every hover-tinted control accumulates highlights.
     *
     * X11 sends LeaveNotify for this; Android does not, so the platform arm
     * synthesises the equivalent. (-1,-1) hit-tests to nothing, so the Window
     * clears hover the same way it does when a real cursor leaves. Queued after
     * the up, so the widget still sees the release while it is still hovered.
     *
     * ANDROID.md Track C item 4. Fixed here rather than in core.vt because
     * "the pointer left" is a platform fact, not a toolkit policy — a desktop
     * must NOT do this. */
    if (e.type == VS_EV_MOUSE_UP) {
        Ev leave = {0};
        leave.type = VS_EV_MOUSE_MOVE;
        leave.x = -1;
        leave.y = -1;
        push_locked(&leave);
    }
    pthread_mutex_unlock(&S.lock);
}

void vs_android_push_key(int keycode, const char *utf8, int mods, int down) {
    Ev e = {0};
    e.type = down ? VS_EV_KEY : VS_EV_KEY_UP;
    e.key  = keycode;
    e.mods = mods;
    /* keycode 0 with text is the "IME committed something" channel that
     * TextField already consumes on desktop via XIM (vsurf.c:2006-2012). */
    if (down && utf8 && *utf8) e.text = strdup(utf8);
    push(&e);
}

/* ------------------------------------------------------------ vsync gating
 *
 * Android composites on its own clock and Canvas.drawBitmap re-uploads the
 * whole bitmap whenever it changed, so presenting more often than the display
 * refreshes is pure waste — and presenting on a 16ms software timer against a
 * 16.67ms display beats, which is visible as periodic judder even when every
 * frame lands inside budget. Choreographer is the only clock that is actually
 * the display's, so the loop presents on it and nothing else.
 *
 * Enabled only while something animates: Vyto's loop blocks when idle, and a
 * free-running frame callback would turn a parked app into a 60Hz wakeup. */

static jmethodID m_set_vsync = NULL;

/* UI thread (Choreographer callback) in, Vyto thread out. At most one
 * VS_EV_VSYNC is ever queued: two would present the same frame twice, and when
 * the Vyto thread is behind it is the *older* one that is stale. */
void vs_android_push_vsync(void) {
    pthread_mutex_lock(&S.lock);
    /* A parked app must stay parked. Choreographer keeps firing for a frame or
     * two after onPause, and queueing those would wake the loop that
     * vs_android_set_paused just put to sleep. */
    if (S.paused || !S.bound) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    int idx = S.head;
    for (int i = 0; i < S.count; i++) {
        if (S.q[idx].type == VS_EV_VSYNC) {
            pthread_mutex_unlock(&S.lock);
            return;
        }
        idx = (idx + 1) % QCAP;
    }
    Ev e = {0};
    e.type = VS_EV_VSYNC;
    push_locked(&e);
    pthread_mutex_unlock(&S.lock);
}

int vs_set_vsync(void *s, int on) {
    (void)s;
    JNIEnv *env = vta_env();
    jobject view = vta_view();
    if (!env || !view) return 0;

    if (!m_set_vsync) {
        /* GetObjectClass, never FindClass: this runs on the Vyto thread, which
         * AttachCurrentThread leaves with no Java frame, so FindClass would
         * resolve against the system loader and never see an app class. Taking
         * the class off an object Java already handed us sidesteps that
         * entirely — same reason the painter and intent shims are safe. */
        jclass c = (*env)->GetObjectClass(env, view);
        m_set_vsync = (*env)->GetMethodID(env, c, "setVsyncEnabled", "(Z)Z");
        (*env)->DeleteLocalRef(env, c);
        if (!m_set_vsync) {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            vta_logf("setVsyncEnabled missing; presenting eagerly");
            return 0;
        }
    }

    /* The View reports back whether it could schedule: it is not attached
     * during teardown, and then there is no Handler to hop to the UI thread
     * with. Passing that failure through is what keeps Window.run() from
     * gating on a clock that will never tick. */
    jboolean ok = (*env)->CallBooleanMethod(env, view, m_set_vsync,
                                            on ? JNI_TRUE : JNI_FALSE);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    return ok == JNI_TRUE ? 1 : 0;
}

void vs_android_push_resize(int w, int h, float density) {
    pthread_mutex_lock(&S.lock);
    S.w = w;
    S.h = h;
    if (density > 0.0f) S.density = density;
    Ev e = {0};
    e.type = VS_EV_RESIZE;
    push_locked(&e);
    pthread_mutex_unlock(&S.lock);
}

void vs_android_push_insets(int l, int t, int r, int b, int ime_h) {
    pthread_mutex_lock(&S.lock);
    int changed = S.ins_l != l || S.ins_t != t || S.ins_r != r
               || S.ins_b != b || S.ins_ime != ime_h;
    S.ins_l = l; S.ins_t = t; S.ins_r = r; S.ins_b = b; S.ins_ime = ime_h;
    /* Announce as a RESIZE. There is no VS_EV_INSETS — the event enum is a
     * wire contract with three other backends — but a layout is exactly what
     * an inset change needs, and RESIZE is already "re-run layout and redraw".
     *
     * Without this the values are dead on arrival: onApplyWindowInsets fires on
     * the UI thread *after* the Vyto thread has already run its first and only
     * layout, so a safe-area widget samples zeroes and nothing ever asks it
     * again. That is why the first labels rendered behind the status bar even
     * though the insets were being delivered correctly.
     *
     * Only on a real change, so the repeated inset callbacks Android issues
     * during a traversal do not each cost a full relayout. */
    if (changed && S.bound) {
        Ev e = {0};
        e.type = VS_EV_RESIZE;
        push_locked(&e);
    }
    pthread_mutex_unlock(&S.lock);
}

/* Read side, for the safe-area layout in vyto/mobile/android/ui. Still no
 * event — a caller samples this during layout, which is exactly when it
 * matters, and the IME case already arrives as a RESIZE because the manifest
 * sets adjustResize. Any NULL out-param is skipped. */
void vs_android_get_insets(int *l, int *t, int *r, int *b, int *ime_h) {
    pthread_mutex_lock(&S.lock);
    if (l)     *l     = S.ins_l;
    if (t)     *t     = S.ins_t;
    if (r)     *r     = S.ins_r;
    if (b)     *b     = S.ins_b;
    if (ime_h) *ime_h = S.ins_ime;
    pthread_mutex_unlock(&S.lock);
}

void vs_android_set_paused(int paused) {
    pthread_mutex_lock(&S.lock);
    S.paused = paused;
    /* Waking on resume matters: a paused vs_wait_timeout ignores its timeout
     * and blocks, so without this the loop would sleep past the resume. */
    if (!paused) pthread_cond_broadcast(&S.cv);
    pthread_mutex_unlock(&S.lock);
}

void vs_android_request_close(void) {
    pthread_mutex_lock(&S.lock);
    S.closing = 1;
    S.paused = 0;              /* a paused loop must still be able to exit */
    Ev e = {0};
    e.type = VS_EV_CLOSE;
    push_locked(&e);
    pthread_cond_broadcast(&S.cv);
    pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------ vsurf: core */

/* The window already exists — it was bound before the Vyto thread started —
 * so this attaches rather than creates. Returns the singleton; there is one
 * surface per process on Android, which the argument-less accessors already
 * assume everywhere. */
void *vs_open(const char *title, int w, int h) {
    (void)title; (void)w; (void)h;
    if (!S.bound) {
        vta_logf("vs_open before bind — Native.start must run first");
        return NULL;
    }
    return &S;
}

void vs_close(void *s) {
    (void)s;
    /* Teardown belongs to the Activity (Native.stop -> vs_android_unbind), not
     * to the app calling close. Nothing to do. */
}

int vs_width(void *s)  { (void)s; return S.w; }
int vs_height(void *s) { (void)s; return S.h; }

void vs_set_title(void *s, const char *t) {
    (void)s; (void)t;
    /* An Android app has no window title. The label in the task switcher comes
     * from the manifest, so this is meaningfully a no-op rather than a gap. */
}

void vs_set_min_size(void *s, int w, int h) {
    (void)s; (void)w; (void)h;
    /* The surface is the size the system gives it — there is no user-draggable
     * frame to clamp. A no-op by nature, not an unimplemented arm. */
}

/* ---------------------------------------------------------- vsurf: events */

static void timespec_in(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);   /* pthread_cond_timedwait's clock */
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int vs_wait(void *s) {
    (void)s;
    Ev e;
    pthread_mutex_lock(&S.lock);
    for (;;) {
        if (pop_locked(&e)) {
            pthread_mutex_unlock(&S.lock);
            return deliver(&e);
        }
        if (!S.bound) {
            pthread_mutex_unlock(&S.lock);
            return VS_EV_CLOSE;
        }
        pthread_cond_wait(&S.cv, &S.lock);
    }
}

int vs_poll(void *s) {
    (void)s;
    Ev e;
    pthread_mutex_lock(&S.lock);
    int got = pop_locked(&e);
    pthread_mutex_unlock(&S.lock);
    return got ? deliver(&e) : VS_EV_NONE;
}

int vs_wait_timeout(void *s, int ms) {
    (void)s;
    Ev e;
    struct timespec ts;
    int timed = 0;

    pthread_mutex_lock(&S.lock);
    for (;;) {
        if (pop_locked(&e)) {
            pthread_mutex_unlock(&S.lock);
            return deliver(&e);
        }
        if (!S.bound) {
            pthread_mutex_unlock(&S.lock);
            return VS_EV_CLOSE;
        }
        if (S.paused) {
            /* Backgrounded: block indefinitely instead of ticking. This is what
             * stops animations burning battery — Window.run() drives tweens by
             * calling wait_timeout(16) in a loop (ui/core.vt:2413), so
             * swallowing the timeout is what parks it. Resume broadcasts. */
            pthread_cond_wait(&S.cv, &S.lock);
            continue;
        }
        if (!timed) { timespec_in(&ts, ms); timed = 1; }
        int rc = pthread_cond_timedwait(&S.cv, &S.lock, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&S.lock);
            last_key = 0; last_mods = 0;
            return VS_EV_TIMER;
        }
    }
}

int vs_key(void *s)   { (void)s; return last_key; }
int vs_mods(void *s)  { (void)s; return last_mods; }
int vs_x(void *s)     { (void)s; return last_x; }
int vs_y(void *s)     { (void)s; return last_y; }
int vs_wheel(void *s) { (void)s; return last_wheel; }

const char *vs_text(void *s) { (void)s; return last_text_buf ? last_text_buf : ""; }

/* --------------------------------------------------------- vsurf: metrics */

long long vs_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* No waitable descriptor: this backend blocks in pthread_cond_timedwait on an
 * event queue the Java side pushes into, not on a socket. An external event
 * loop cannot select() on a condvar, so -1 is the honest answer and the caller
 * falls back to a bounded vs_wait_timeout — which is also what keeps the
 * paused-app battery behaviour intact (vs_wait_timeout swallows the timeout
 * while backgrounded; an fd-driven loop outside would not know to). */
int vs_event_fd(void *s) { (void)s; return -1; }

/* No damage region. Android drives repaints through AndroidPainter and the
 * command buffer, not Expose, so there is nothing to report: 0 means "repaint
 * everything", which is what this path already did. */
int vs_damage(void *s, VsRect *out) {
    (void)s; (void)out;
    return 0;
}

/* Cannot fold caller fds into a condvar wait, so this degrades to the bounded
 * wait — the same 16ms polling the caller would otherwise do by hand.
 *
 * Deliberately NOT reimplemented as a select() over the fds alone: that would
 * lose the paused-app behaviour above, where vs_wait_timeout blocks
 * indefinitely instead of ticking so a backgrounded app stops burning battery.
 * Correctness of the lifecycle beats power on the path that is already the
 * fallback. */
int vs_wait_timeout_fds(void *s, int ms, const int *fds, int n) {
    (void)fds; (void)n;
    return vs_wait_timeout(s, ms);
}

int vs_can_wait_fds(void *s) { (void)s; return 0; }

/* Events already queued by the Java side, without touching the condvar. */
int vs_events_pending(void *s) {
    (void)s;
    int n;
    pthread_mutex_lock(&S.lock);
    n = S.count;
    pthread_mutex_unlock(&S.lock);
    return n;
}

int vs_scale_pct(void) {
    /* $VYTO_SCALE first, matching every other backend (vsurf.c:50-57), so a
     * device can be forced to a known scale for screenshot comparison. */
    const char *env = getenv("VYTO_SCALE");
    if (env && *env) {
        double v = atof(env);
        if (v > 0.0) return (int)(v < 10.0 ? v * 100.0 : v);
    }
    /* Android's density is already a 160dpi-baseline multiplier; vsurf's
     * baseline is 96dpi, but Theme.apply_scale wants "how much bigger than a
     * desktop pixel", which is what density means. Using it directly is
     * correct, not a conversion bug. */
    int pct = (int)(S.density * 100.0f + 0.5f);
    return pct > 0 ? pct : 100;
}

/* ------------------------------------------------------- vsurf: drawing */
/*
 * All no-ops. Android renders through AndroidPainter, which bypasses these
 * entirely and talks to its own command buffer; these exist only because the
 * header contract requires them.
 *
 * The consequence is that the LEAN tier (SurfacePainter, which draws via these
 * calls) renders nothing on Android. That is intended — AndroidPainter is the
 * only supported painter here — but it fails silently, so the first call logs
 * once rather than leaving a blank screen unexplained.
 */
static void lean_tier_warning(void) {
    static int warned = 0;
    if (warned) return;
    warned = 1;
    vta_logf("surface draw call on Android: the lean tier (SurfacePainter) "
             "draws nothing here. Use AndroidPainter — win.use_painter(...)");
}

void vs_fill_rect(void *s, int x, int y, int w, int h, int rgb) {
    (void)s;(void)x;(void)y;(void)w;(void)h;(void)rgb; lean_tier_warning();
}
void vs_draw_rect(void *s, int x, int y, int w, int h, int rgb) {
    (void)s;(void)x;(void)y;(void)w;(void)h;(void)rgb; lean_tier_warning();
}
void vs_draw_line(void *s, int x0, int y0, int x1, int y1, int rgb) {
    (void)s;(void)x0;(void)y0;(void)x1;(void)y1;(void)rgb; lean_tier_warning();
}
void vs_draw_text(void *s, int x, int y, const char *str, int rgb) {
    (void)s;(void)x;(void)y;(void)str;(void)rgb; lean_tier_warning();
}
void vs_blit(void *s, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth) {
    (void)s;(void)pixels;(void)srcw;(void)srch;
    (void)dstx;(void)dsty;(void)dstw;(void)dsth; lean_tier_warning();
}
void vs_blit_rect(void *s, const int *pixels, int stride_px,
                  int srcx, int srcy, int w, int h, int dstx, int dsty) {
    (void)s;(void)pixels;(void)stride_px;(void)srcx;(void)srcy;
    (void)w;(void)h;(void)dstx;(void)dsty; lean_tier_warning();
}
void vs_present(void *s)      { (void)s; }
void vs_present_rect(void *s, int x, int y, int w, int h) {
    (void)s;(void)x;(void)y;(void)w;(void)h;
}
void vs_clip_set(void *s, int x, int y, int w, int h) {
    (void)s;(void)x;(void)y;(void)w;(void)h;
}
void vs_clip_clear(void *s) { (void)s; }

/* Font metrics belong to AndroidPainter's Paint, which this file has no access
 * to. Returning plausible constants keeps a lean-tier layout from dividing by
 * zero; it will still draw nothing. */
int vs_text_width(void *s, const char *str) {
    (void)s;
    return str ? (int)(strlen(str) * 8) : 0;
}
int vs_font_ascent(void *s) { (void)s; return 12; }
int vs_font_height(void *s) { (void)s; return 16; }

/* ------------------------------------------------------- vsurf: clipboard */
/*
 * Process-local, like the headless and fbdev backends (vsurf.c:64-72).
 *
 * TODO: route through android.content.ClipboardManager via JNI. Nothing here
 * blocks it — Actions already proves the up-call pattern — but a local buffer
 * means copy/paste does not cross app boundaries, which users will notice.
 */
static char *clip_local = NULL;

void vs_clipboard_set(void *s, const char *text) {
    (void)s;
    free(clip_local);
    clip_local = text ? strdup(text) : NULL;
}

const char *vs_clipboard_get(void *s) {
    (void)s;
    return clip_local ? clip_local : "";
}

/* ------------------------------------------------------ vsurf: escape hatch */

void *vs_native_display(void *s) {
    (void)s;
    return (void *)vta_view();   /* the VytoView global ref */
}

unsigned long vs_native_window(void *s) { (void)s; return 0; }
void *vs_native_gc(void *s)             { (void)s; return NULL; }

#endif /* __ANDROID__ */
