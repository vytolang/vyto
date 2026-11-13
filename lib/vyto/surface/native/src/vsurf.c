/* vsurf backends:
 *   - Win32 GDI when compiling for Windows (_WIN32)
 *   - X11 everywhere else
 *   - headless (VS_HEADLESS=1): no window system at all; events replay from
 *     the $VS_EVENTS script. Shared by every platform, used by the tests.
 *   - Android: NOT here. See vsurf_android.c, which implements the whole
 *     header on its own; this file compiles to nothing under __ANDROID__.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */

/* Windows API level, and it has to be set before ANY include: mingw's _mingw.h
   (reached via <stdio.h>) defaults it to the XP-era value, which hides
   GetTickCount64 (Vista+). Left implicitly declared, that call returns int and
   the top 32 bits of the tick count are lost, so vs_now_ms jumps backwards once
   the host has been up 24.9 days. Vista is the floor for the Win32 backend.
   Inert on non-Windows builds. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "vsurf.h"

/* Android gets its own file. Guarding the whole body rather than adding a
   third arm at the _WIN32 split below, because the split is not the only
   platform-dependent part: the shared region above it defines the public
   vs_key/vs_text/vs_x/vs_y/vs_wheel/vs_mods accessors, which would collide at
   link time with the ones in vsurf_android.c. (The file-static globals behind
   them would not collide — internal linkage — but the accessors are external.)

   Placed after the vsurf.h include so the translation unit still contains
   declarations on Android instead of being empty, which ISO C forbids.

   vytoc compiles every .c in native/src on every target, so vsurf_android.c is
   likewise wrapped in #ifdef __ANDROID__ and costs nothing here. */
#ifndef __ANDROID__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int headless_on(void);
static long long headless_ms; /* synthetic clock: +16 per scripted event */

/* Per-surface input state. Each arm's VSurf embeds one of these, so the shared
   headless reader can fill a surface's slot without knowing that arm's struct
   layout — the platform structs are defined below the #ifdef split, but this is
   not. Was a set of file-scope statics, which two windows would have shared.
   `text` is the storage vs_text() hands back a pointer to, so it lives exactly
   as long as its surface. */
typedef struct VsInput {
    int key, x, y, wheel, mods;
    int wheel_x;     /* horizontal wheel delta of the last wheel event */
    int button;      /* VS_BUTTON_* of the last BUTTON_DOWN/UP */
    int cursor;      /* VS_CURSOR_* last set; what vs_get_cursor reports */
    char text[32];
    int pending_up;  /* headless: a click's MOUSE_UP still owed to this surface */
    int hl_index;    /* headless: this surface's open order, what `win N` names */
    int dpi_pct;     /* the window's scale as the platform last reported it
                        (Win32 WM_DPICHANGED, headless `scale N`); 0 = never
                        reported, so vs_scale_pct asks the monitor instead */
    int scale_changed; /* set with dpi_pct, cleared by vs_take_scale_change */
} VsInput;

/* Headless multi-window routing. The event script is one process-global reader,
   but vs_wait/vs_poll are called PER SURFACE — so a scripted event has to say
   which window it is for, and a surface whose turn it is not must not consume
   it. `win N` sets the target; surfaces are numbered in vs_open order, and the
   default of 0 is what keeps every existing script working unchanged. */
static int hl_target;    /* window index the script is currently addressing */
static int hl_next_index; /* open-order counter handed out by vs_open */

#ifdef _WIN32
#include <windows.h>
long long vs_now_ms(void) {
    if (headless_on()) return headless_ms;
    return (long long)GetTickCount64();
}
#else
#include <time.h>
long long vs_now_ms(void) {
    /* headless: deterministic 16ms-per-event clock so animation tests
       replay identically regardless of host speed */
    if (headless_on()) return headless_ms;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

static int g_scale_pct; /* cached DPI scale (percent), set at vs_open */

/* One $VYTO_SCALE entry as a percent, or 0 when it is not usable. Accepts a
   factor ("1.5") or a percent ("150"); the 8.0 split is what tells them apart. */
static int scale_parse_one(const char *e) {
    if (!e || !*e) return 0;
    double f = atof(e);
    if (f > 8.0) return (int)(f + 0.5); /* given as percent already */
    if (f >= 0.5) return (int)(f * 100.0 + 0.5);
    return 0;
}

/* $VYTO_SCALE overrides platform DPI detection. One value applies everywhere;
   a comma list names each monitor in order ("100,200"), which is how mixed-DPI
   behaviour is exercised on a machine with a single screen. `index` picks an
   entry; past the end of a list, the LAST entry stands, so "150" and "150,150"
   mean the same thing for any monitor count. */
static int scale_from_env_at(int index) {
    const char *e = getenv("VYTO_SCALE");
    if (!e || !*e) return 0;
    if (index < 0) index = 0;
    const char *p = e;
    int got = 0;
    for (int i = 0; ; i++) {
        int v = scale_parse_one(p);
        if (v > 0) got = v;         /* remember the last usable entry */
        if (i >= index) break;
        const char *c = strchr(p, ',');
        if (!c) break;              /* list ran out: keep the last one */
        p = c + 1;
    }
    return got;
}

/* The scale with no monitor in mind — the first entry, and the whole value for
   the ordinary scalar form. */
static int scale_from_env(void) { return scale_from_env_at(0); }

/* Index of the monitor containing (x, y), or 0 when nothing does. Used to give
   a window the scale of the screen it is actually on. Declared here because
   both desktop arms need it; vs_monitors is defined per arm below. */
static int monitor_index_at(int x, int y) {
    VsMonitor mons[16];
    int n = vs_monitors(mons, 16);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        if (x >= mons[i].x && x < mons[i].x + mons[i].w &&
            y >= mons[i].y && y < mons[i].y + mons[i].h) {
            return i;
        }
    }
    return 0;   /* off every screen (or none reported): the first one */
}


/* process-local clipboard buffer: the whole story for headless and fbdev,
   and the "we own the selection" copy on X11 */
static char *clip_local;

static void clip_store(char **slot, const char *text) {
    size_t n = strlen(text);
    char *p = realloc(*slot, n + 1);
    if (!p) return;
    memcpy(p, text, n + 1);
    *slot = p;
}

/* named-key table shared by the headless script parser; also the single
   place a new VS_KEY_* needs a script-facing name */
static const struct { const char *name; int key; } key_names[] = {
    {"Space", VS_KEY_SPACE},
    {"Enter", VS_KEY_ENTER}, {"Backspace", VS_KEY_BACKSPACE}, {"Esc", VS_KEY_ESC},
    {"Up", VS_KEY_UP}, {"Down", VS_KEY_DOWN}, {"Left", VS_KEY_LEFT}, {"Right", VS_KEY_RIGHT},
    {"Delete", VS_KEY_DELETE}, {"Tab", VS_KEY_TAB}, {"Home", VS_KEY_HOME}, {"End", VS_KEY_END},
    {"PageUp", VS_KEY_PAGEUP}, {"PageDown", VS_KEY_PAGEDOWN}, {"Insert", VS_KEY_INSERT},
    {"Shift", VS_KEY_SHIFT}, {"Ctrl", VS_KEY_CTRL}, {"Alt", VS_KEY_ALT}, {"Super", VS_KEY_SUPER},
    {"F1", VS_KEY_F1}, {"F2", VS_KEY_F2}, {"F3", VS_KEY_F3}, {"F4", VS_KEY_F4},
    {"F5", VS_KEY_F5}, {"F6", VS_KEY_F6}, {"F7", VS_KEY_F7}, {"F8", VS_KEY_F8},
    {"F9", VS_KEY_F9}, {"F10", VS_KEY_F10}, {"F11", VS_KEY_F11}, {"F12", VS_KEY_F12},
};

static int key_by_name(const char *n) {
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (strcmp(key_names[i].name, n) == 0) return key_names[i].key;
    return 0;
}

/* ---- headless backend (platform-independent) ---- */

static int headless_on(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("VS_HEADLESS");
        cached = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static FILE *script; /* $VS_EVENTS */
static char script_line[512];
static const char *type_p; /* rest of a "type ..." line being replayed */

static void headless_open_script(void) {
    const char *ev = getenv("VS_EVENTS");
    if (ev && !script) script = fopen(ev, "r");
}

/* one directive per line:
 *   type <text>      each character becomes a key event
 *   key <name>       Enter Backspace Esc Up Down Left Right Delete Tab Home End
 *   keyup <name>     same names, delivered as a key-release (VS_EV_KEY_UP)
 *   click X Y        mouse down (then up) at X,Y
 *   down X Y         mouse down at X,Y, no paired up — for drag-hold tests;
 *                    pair with a later "up X Y"
 *   up X Y           mouse up at X,Y
 *   rclick X Y       right-button mouse down at X,Y (VS_EV_MOUSE_RDOWN) with no
 *                    release, as before the release existed, so every older
 *                    script replays identically (each event advances the
 *                    synthetic clock); script the release as "button up right"
 *   button down|up <right|middle|back|forward> X Y
 *                    one press or release of a non-left button: BUTTON_DOWN /
 *                    BUTTON_UP with vs_button() set ("button down right" is
 *                    MOUSE_RDOWN, as on the real backends)
 *   move X Y         the pointer moved (VS_EV_MOUSE_MOVE)
 *   scroll N [X Y]   vertical wheel, N lines (positive = down)
 *   hscroll N [X Y]  horizontal wheel, N columns (positive = right)
 *   resize W H
 *   scale PCT        the window moved to a display of a different scale, as
 *                    Win32's WM_DPICHANGED reports it: vs_scale_pct answers
 *                    PCT from now on and the event is a RESIZE at the same
 *                    size, with vs_take_scale_change() true once
 *   expose
 *   tick             a timer tick (VS_EV_TIMER), for game-loop tests
 *   mods <spec>      set the modifier state reported by vs_mods() for all
 *                    following events: "+"-joined ctrl/shift/alt/super, or
 *                    "none" (e.g. "mods ctrl+shift"). Emits no event itself.
 *   clip <text>      seed the process-local clipboard (what vs_clipboard_get
 *                    returns) — the paste-test hook. Emits no event itself.
 *   win <n>          address window <n> (open order, 0-based) with everything
 *                    that follows. Emits no event itself. Absent means 0, so a
 *                    single-window script needs no `win` line — which is why
 *                    every pre-existing script still behaves identically.
 *                    Indices are open order, so closing and reopening a surface
 *                    renumbers: fine for a scripted test, not a general API.
 *   close
 * '#' starts a comment; EOF acts as close. */
static int headless_wait(VsInput *in, int *w, int *h) {
    headless_ms += 16;     /* one synthetic frame per delivered event */
    /* Not this window's turn: report "nothing for me" rather than draining an
       event addressed to another surface. The caller polls both. */
    if (in->hl_index != hl_target) return VS_EV_NONE;
    if (in->pending_up) {
        in->pending_up = 0;
        return VS_EV_MOUSE_UP;
    }
    for (;;) {
        if (type_p && *type_p) {
            /* one key event per UTF-8 sequence; multibyte input has no
               ASCII key code (key = 0), only insertable text */
            unsigned char lead = (unsigned char)*type_p;
            int len = 1;
            if ((lead & 0xE0) == 0xC0) len = 2;
            else if ((lead & 0xF0) == 0xE0) len = 3;
            else if ((lead & 0xF8) == 0xF0) len = 4;
            in->key = len == 1 ? (int)lead : 0;
            int i = 0;
            while (i < len && type_p[i]) { in->text[i] = type_p[i]; i++; }
            in->text[i] = 0;
            type_p += i;
            return VS_EV_KEY;
        }
        type_p = NULL;
        if (!script || !fgets(script_line, sizeof script_line, script)) return VS_EV_CLOSE;
        char *nl = strchr(script_line, '\n');
        if (nl) *nl = 0;
        if (!script_line[0] || script_line[0] == '#') continue;
        if (strncmp(script_line, "type ", 5) == 0) {
            type_p = script_line + 5;
            continue;
        }
        if (strncmp(script_line, "key ", 4) == 0 || strncmp(script_line, "keyup ", 6) == 0) {
            int up = script_line[3] == 'u';
            const char *k = script_line + (up ? 6 : 4);
            in->text[0] = 0;
            int code = key_by_name(k);
            if (!code) continue;
            in->key = code;
            if (code == VS_KEY_SPACE) { in->text[0] = ' '; in->text[1] = 0; }
            return up ? VS_EV_KEY_UP : VS_EV_KEY;
        }
        {
            int wn;
            if (sscanf(script_line, "win %d", &wn) == 1) {
                hl_target = wn;
                /* Stop reading HERE rather than continuing the loop: everything
                   after this line belongs to another window, and this surface
                   would otherwise consume the very events it just handed over.
                   The caller polls each surface, so the new target picks up on
                   its own next call. */
                if (in->hl_index != hl_target) return VS_EV_NONE;
                continue;
            }
        }
        if (strncmp(script_line, "clip ", 5) == 0) {
            clip_store(&clip_local, script_line + 5);
            continue;
        }
        if (strncmp(script_line, "mods ", 5) == 0) {
            const char *p = script_line + 5;
            int m = 0;
            while (*p) {
                if (strncmp(p, "ctrl", 4) == 0) { m |= VS_MOD_CTRL; p += 4; }
                else if (strncmp(p, "shift", 5) == 0) { m |= VS_MOD_SHIFT; p += 5; }
                else if (strncmp(p, "alt", 3) == 0) { m |= VS_MOD_ALT; p += 3; }
                else if (strncmp(p, "super", 5) == 0) { m |= VS_MOD_SUPER; p += 5; }
                else if (strncmp(p, "none", 4) == 0) { m = 0; p += 4; }
                else p++;
            }
            in->mods = m;
            continue;
        }
        if (sscanf(script_line, "click %d %d", &in->x, &in->y) == 2) {
            in->pending_up = 1;
            return VS_EV_MOUSE_DOWN;
        }
        if (sscanf(script_line, "rclick %d %d", &in->x, &in->y) == 2) {
            return VS_EV_MOUSE_RDOWN;
        }
        {
            char dir[8], name[16];
            int bx, by;
            if (sscanf(script_line, "button %7s %15s %d %d", dir, name, &bx, &by) == 4) {
                int b = strcmp(name, "right") == 0 ? VS_BUTTON_RIGHT
                      : strcmp(name, "middle") == 0 ? VS_BUTTON_MIDDLE
                      : strcmp(name, "back") == 0 ? VS_BUTTON_BACK
                      : strcmp(name, "forward") == 0 ? VS_BUTTON_FORWARD : 0;
                int down = strcmp(dir, "down") == 0;
                if (!b || (!down && strcmp(dir, "up") != 0)) continue;
                in->x = bx;
                in->y = by;
                in->button = b;
                if (down && b == VS_BUTTON_RIGHT) return VS_EV_MOUSE_RDOWN;
                return down ? VS_EV_BUTTON_DOWN : VS_EV_BUTTON_UP;
            }
        }
        if (sscanf(script_line, "down %d %d", &in->x, &in->y) == 2) {
            return VS_EV_MOUSE_DOWN;
        }
        if (sscanf(script_line, "up %d %d", &in->x, &in->y) == 2) {
            return VS_EV_MOUSE_UP;
        }
        {
            int nw, nh;
            if (sscanf(script_line, "resize %d %d", &nw, &nh) == 2) {
                *w = nw;
                *h = nh;
                return VS_EV_RESIZE;
            }
            int pct;
            if (sscanf(script_line, "scale %d", &pct) == 1) {
                if (pct < 25 || pct > 800) continue;
                in->dpi_pct = pct;
                in->scale_changed = 1;
                return VS_EV_RESIZE;
            }
        }
        if (strcmp(script_line, "expose") == 0) return VS_EV_EXPOSE;
        if (strcmp(script_line, "tick") == 0) return VS_EV_TIMER;
        if (strcmp(script_line, "close") == 0) return VS_EV_CLOSE;
        {
            int mx, my;
            if (sscanf(script_line, "move %d %d", &mx, &my) == 2) {
                in->x = mx;
                in->y = my;
                return VS_EV_MOUSE_MOVE;
            }
        }
        {
            int sc, sx, sy;
            int horiz = strncmp(script_line, "hscroll ", 8) == 0;
            const char *sl = script_line + (horiz ? 1 : 0);
            if (sscanf(sl, "scroll %d %d %d", &sc, &sx, &sy) == 3) {
                in->wheel = horiz ? 0 : sc;
                in->wheel_x = horiz ? sc : 0;
                in->x = sx;
                in->y = sy;
                return VS_EV_MOUSE_WHEEL;
            }
            if (sscanf(sl, "scroll %d", &sc) == 1) {
                in->wheel = horiz ? 0 : sc;
                in->wheel_x = horiz ? sc : 0;
                return VS_EV_MOUSE_WHEEL;
            }
        }
        /* unknown directive: skip */
    }
}

/* The one monitor a machine with no window system still has to describe.
   surface_monitors() must never come back empty — every caller indexes [0]
   without guarding — so headless (and an X11 arm that cannot open a display)
   answers with a synthetic screen rather than reporting nothing.
   1920x1080 because a headless layout test wants a plausible desktop, not a
   degenerate one; $VYTO_SCALE still decides the scale, matching what
   vs_scale_pct() reports for a headless surface. */
static int headless_monitors(VsMonitor *out, int max) {
    if (max > 0 && out) {
        out[0].x = 0;
        out[0].y = 0;
        out[0].w = 1920;
        out[0].h = 1080;
        out[0].primary = 1;
        int sc = scale_from_env_at(0);
        out[0].scale_pct = sc > 0 ? sc : 100;
    }
    return 1;
}

/* The six input accessors used to live here, reading file-scope statics. They
   now read per-surface fields, and VSurf is defined separately in each arm
   below — so the definitions moved down into the Win32 and X11 arms. See
   vs_input_accessors in each. */

#ifdef _WIN32
/* ================================================================ Win32 */

#include <windows.h>
#include <imm.h>
#include "vyto_win.h" /* UTF-8 <-> UTF-16 for titles and GDI text */

typedef struct QEv {
    int type, key, x, y, mods;
    int button, wheel_x;
    char text[8]; /* one code point as UTF-8, NUL-terminated */
} QEv;

/* held-button mask, for capture: SetCapture on the first press, release once
   nothing is held */
#define VS_HELD_LEFT   1
#define VS_HELD_RIGHT  2
#define VS_HELD_MIDDLE 4
#define VS_HELD_X1     8
#define VS_HELD_X2     16

typedef struct VSurf {
    HWND hwnd; /* NULL in headless mode */
    HDC memdc; /* backbuffer */
    HBITMAP bmp;
    HBITMAP bmp0; /* original 1x1 bitmap of memdc, restored before delete */
    uint32_t *bits; /* the backbuffer's pixels (a DIB section), or NULL when
                       only a device-dependent bitmap could be made */
    HFONT font;
    HICON icon;   /* from vs_set_icon, owned here; NULL = class/exe icon */
    HCURSOR cursor; /* shown over the client area; NULL = hidden */
    int w, h;
    int min_w, min_h; /* client-area floor from vs_set_min_size; 0 = none */
    WCHAR hi_surrogate; /* first half of a UTF-16 pair awaiting its second */
    int wheel_acc, hwheel_acc; /* sub-notch wheel remainders (touchpads) */
    int held;          /* VS_HELD_* buttons currently down */
    int last_x, last_y; /* last pointer position, for a synthesized release */

    VsInput in;   /* this window's input state; see VsInput above */
    QEv evq[64];  /* this window's pending events */
    int evq_head, evq_len;
} VSurf;

/* WndProc -> vs_wait event queue. One ring PER SURFACE: a shared ring let
   whichever window called vs_wait pop another window's events. */

/* async modifier state at event time */
static int win_mods(void) {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= VS_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= VS_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= VS_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= VS_MOD_SUPER;
    return m;
}

/* Queue one event and return it, so a caller can fill the fields only some
   events carry (button, wheel_x, text). NULL when nothing was queued: no
   surface yet, a full ring, or a move folded into the one before it. */
static QEv *q_push(VSurf *s, int type, int key, int x, int y) {
    /* Messages arrive before vs_open's SetWindowLongPtr runs (WM_NCCREATE,
       WM_CREATE), so GWLP_USERDATA is still null then. Dropping those is
       correct: there is no surface yet for them to belong to. */
    if (!s) return NULL;
    /* coalesce mouse-move bursts: overwrite a trailing queued move */
    if (type == VS_EV_MOUSE_MOVE && s->evq_len > 0) {
        QEv *tail = &s->evq[(s->evq_head + s->evq_len - 1) % 64];
        if (tail->type == VS_EV_MOUSE_MOVE) {
            tail->x = x;
            tail->y = y;
            tail->mods = win_mods();
            return NULL;
        }
    }
    if (s->evq_len == 64) {
        /* drop when full — except CLOSE, which must never be lost */
        if (type != VS_EV_CLOSE) return NULL;
        s->evq_len--; /* sacrifice the newest queued event for the CLOSE */
    }
    QEv *e = &s->evq[(s->evq_head + s->evq_len) % 64];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->key = key;
    e->x = x;
    e->y = y;
    e->mods = win_mods();
    s->evq_len++;
    return e;
}

/* one code point -> UTF-8, NUL-terminated; out holds at least 5 bytes */
static void utf8_put(unsigned cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp; out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F)); out[2] = 0;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); out[3] = 0;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F)); out[4] = 0;
    }
}

/* Implicit capture is not enough: Windows only captures for the left button
   during its own drag, and a press whose release lands outside the window is
   otherwise never released at all. Capture on the first press, release when
   the last button comes up. */
static void btn_press(VSurf *s, HWND hwnd, int bit, LPARAM lp) {
    if (!s) return;
    if (!s->held) SetCapture(hwnd);
    s->held |= bit;
    s->last_x = (short)LOWORD(lp);
    s->last_y = (short)HIWORD(lp);
}
static void btn_release(VSurf *s, int bit, LPARAM lp) {
    if (!s) return;
    s->held &= ~bit;
    s->last_x = (short)LOWORD(lp);
    s->last_y = (short)HIWORD(lp);
    if (!s->held && GetCapture() == s->hwnd) ReleaseCapture();
}
static void q_button(VSurf *s, int type, int button, LPARAM lp) {
    QEv *e = q_push(s, type, 0, (short)LOWORD(lp), (short)HIWORD(lp));
    if (e) e->button = button;
}

/* shared VK -> VS_KEY_* mapping for WM_KEYDOWN/WM_KEYUP (and SYS variants) */
static int vk_to_key(WPARAM wp) {
    switch (wp) {
    case VK_RETURN: return VS_KEY_ENTER;
    case VK_BACK: return VS_KEY_BACKSPACE;
    case VK_ESCAPE: return VS_KEY_ESC;
    case VK_UP: return VS_KEY_UP;
    case VK_DOWN: return VS_KEY_DOWN;
    case VK_LEFT: return VS_KEY_LEFT;
    case VK_RIGHT: return VS_KEY_RIGHT;
    case VK_DELETE: return VS_KEY_DELETE;
    case VK_TAB: return VS_KEY_TAB;
    case VK_HOME: return VS_KEY_HOME;
    case VK_END: return VS_KEY_END;
    case VK_PRIOR: return VS_KEY_PAGEUP;
    case VK_NEXT: return VS_KEY_PAGEDOWN;
    case VK_INSERT: return VS_KEY_INSERT;
    case VK_SHIFT: return VS_KEY_SHIFT;
    case VK_CONTROL: return VS_KEY_CTRL;
    case VK_MENU: return VS_KEY_ALT;
    case VK_LWIN: case VK_RWIN: return VS_KEY_SUPER;
    default:
        if (wp >= VK_F1 && wp <= VK_F12) return VS_KEY_F1 + (int)(wp - VK_F1);
        return 0;
    }
}

/* 0xRRGGBB -> COLORREF (0x00BBGGRR) */
static COLORREF cref(int rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/* The backbuffer is a DIB section: GDI draws into it as before, and the
   painters' pixels are copied straight into its memory (vs_blit_rect) instead
   of going through StretchDIBits and a temporary — that path was about half
   of every windowed frame on Windows 7. A device-dependent bitmap is kept as
   the fallback for the case a DIB section cannot be made. */
static void make_backbuffer(VSurf *s) {
    HDC wdc = GetDC(s->hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    int bw = s->w > 0 ? s->w : 1, bh = s->h > 0 ? s->h : 1;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = bw;
    bmi.bmiHeader.biHeight = -bh; /* top-down: row 0 is y = 0 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32; /* 0x00RRGGBB per pixel, our layout */
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(wdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) {
        bits = NULL;
        bmp = CreateCompatibleBitmap(wdc, bw, bh);
    }
    s->bits = (uint32_t *)bits;
    HBITMAP b0 = SelectObject(mem, bmp);
    SelectObject(mem, s->font);
    SetBkMode(mem, TRANSPARENT);
    if (s->memdc) {
        SelectObject(s->memdc, s->bmp0);
        DeleteObject(s->bmp);
        DeleteDC(s->memdc);
    }
    s->memdc = mem;
    s->bmp = bmp;
    s->bmp0 = b0;
    ReleaseDC(s->hwnd, wdc);
}

static LRESULT CALLBACK vs_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    VSurf *s = (VSurf *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (s && s->memdc) BitBlt(dc, 0, 0, s->w, s->h, s->memdc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        q_push(s, VS_EV_EXPOSE, 0, 0, 0);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (s && w > 0 && h > 0 && (w != s->w || h != s->h)) {
            s->w = w;
            s->h = h;
            make_backbuffer(s);
            q_push(s, VS_EV_RESIZE, 0, 0, 0);
        }
        return 0;
    }
    case 0x02E0: /* WM_DPICHANGED: Windows 8.1+, and only to a per-monitor
                    aware process (the manifest asks for that; Windows 7
                    reads it as system aware and never sends this). */
        if (s) {
            int dpi = LOWORD(wp);
            if (dpi >= 24) {
                s->in.dpi_pct = (dpi * 100 + 48) / 96;
                s->in.scale_changed = 1;
            }
            /* Take the rect Windows suggests: it keeps the window where the
               user dropped it, at the size the new scale implies. That runs
               WM_SIZE, which remakes the backbuffer and queues the RESIZE
               Window.run() reacts to. At the same size (a rect Windows did
               not change) nothing would be queued, so queue it here. */
            int ow = s->w, oh = s->h;
            const RECT *r = (const RECT *)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (s->w == ow && s->h == oh) q_push(s, VS_EV_RESIZE, 0, 0, 0);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (s && s->min_w > 0 && s->min_h > 0) {
            /* min_w/min_h are client-area, but this message wants the whole
               window, so grow them by the frame the current style adds. */
            RECT r = { 0, 0, s->min_w, s->min_h };
            AdjustWindowRect(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        /* Claim the erase so GDI never paints a background under us. The class
           brush is null today, so this changes nothing — it is here so that
           setting one later cannot reintroduce a flash on resize. */
        return 1;
    case WM_CLOSE:
        q_push(s, VS_EV_CLOSE, 0, 0, 0);
        return 0; /* the Vyto loop decides; vs_close destroys the window */
    case WM_LBUTTONDOWN:
        btn_press(s, hwnd, VS_HELD_LEFT, lp);
        q_push(s, VS_EV_MOUSE_DOWN, 0, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        btn_release(s, VS_HELD_LEFT, lp);
        q_push(s, VS_EV_MOUSE_UP, 0, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_RBUTTONDOWN:
        btn_press(s, hwnd, VS_HELD_RIGHT, lp);
        q_push(s, VS_EV_MOUSE_RDOWN, 0, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_RBUTTONUP:
        btn_release(s, VS_HELD_RIGHT, lp);
        q_button(s, VS_EV_BUTTON_UP, VS_BUTTON_RIGHT, lp);
        return 0;
    case WM_MBUTTONDOWN:
        btn_press(s, hwnd, VS_HELD_MIDDLE, lp);
        q_button(s, VS_EV_BUTTON_DOWN, VS_BUTTON_MIDDLE, lp);
        return 0;
    case WM_MBUTTONUP:
        btn_release(s, VS_HELD_MIDDLE, lp);
        q_button(s, VS_EV_BUTTON_UP, VS_BUTTON_MIDDLE, lp);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        int x1 = GET_XBUTTON_WPARAM(wp) == XBUTTON1;
        int bit = x1 ? VS_HELD_X1 : VS_HELD_X2;
        int down = msg == WM_XBUTTONDOWN;
        if (down) btn_press(s, hwnd, bit, lp);
        else btn_release(s, bit, lp);
        q_button(s, down ? VS_EV_BUTTON_DOWN : VS_EV_BUTTON_UP,
                 x1 ? VS_BUTTON_BACK : VS_BUTTON_FORWARD, lp);
        return TRUE; /* the X-button messages want TRUE when handled */
    }
    case WM_CAPTURECHANGED:
        /* Capture taken away mid-press (another window grabbed it, a modal
           loop started): the release will never arrive, so deliver it now at
           the last known position, or the drag stays stuck down. Our own
           ReleaseCapture in btn_release has already cleared `held`. */
        if (s && (HWND)lp != hwnd && s->held) {
            if (s->held & VS_HELD_LEFT)
                q_push(s, VS_EV_MOUSE_UP, 0, s->last_x, s->last_y);
            s->held = 0;
        }
        return 0;
    case WM_SETCURSOR:
        /* Our shape over the client area only; the frame's resize arrows are
           DefWindowProc's. */
        if (s && LOWORD(lp) == HTCLIENT) {
            SetCursor(s->cursor);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (s) { s->last_x = (short)LOWORD(lp); s->last_y = (short)HIWORD(lp); }
        q_push(s, VS_EV_MOUSE_MOVE, 0, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        if (!s) return 0;
        /* One notch is WHEEL_DELTA (120) and maps to 3 lines. A touchpad
           sends a fraction of that per message, so keep the remainder: a
           plain delta*3/120 rounds every one of those to 0 and the pad
           never scrolls. */
        int horiz = msg == WM_MOUSEHWHEEL;
        int *acc = horiz ? &s->hwheel_acc : &s->wheel_acc;
        *acc += GET_WHEEL_DELTA_WPARAM(wp) * 3;
        int lines = *acc / WHEEL_DELTA;
        *acc -= lines * WHEEL_DELTA;
        if (lines) {
            /* Vertical: Windows counts rotation away from the user as
               positive, which scrolls UP; the contract is positive = down
               (as X11's Button5), so flip it. Horizontal: tilt right is
               positive, which already matches. */
            QEv *e = q_push(s, VS_EV_MOUSE_WHEEL, horiz ? 0 : -lines, pt.x, pt.y);
            if (e && horiz) e->wheel_x = lines;
        }
        return horiz ? TRUE : 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: { /* SYS variant: Alt itself and Alt-held chords */
        int k = vk_to_key(wp);
        /* AltGr arrives as Ctrl+Alt (Windows synthesizes the left Ctrl), and
           it types characters (@, €, { on German, French, Polish layouts).
           Not a shortcut, then: leave it to WM_CHAR. */
        int altgr = (GetKeyState(VK_RMENU) & 0x8000) != 0;
        if (!k && !altgr && (GetKeyState(VK_CONTROL) & 0x8000)) {
            /* Ctrl chords produce control-char WM_CHARs; report the plain
               key here instead so shortcuts see 'a', not 0x01 (no text) */
            if (wp >= 'A' && wp <= 'Z') k = (int)wp + 32;
            else if (wp >= '0' && wp <= '9') k = (int)wp;
            else if (wp == VK_SPACE) k = VS_KEY_SPACE;
        }
        if (k) q_push(s, VS_EV_KEY, k, 0, 0);
        /* unhandled keys fall to DefWindowProc (TranslateMessage yields
           WM_CHAR); SYS messages always fall through so Alt+F4 etc. work */
        if (k && msg == WM_KEYDOWN) return 0;
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int k = vk_to_key(wp);
        if (!k) {
            /* no WM_CHAR on release: report printable letters/digits/space by VK */
            if (wp >= 'A' && wp <= 'Z') k = (int)wp + 32;
            else if (wp >= '0' && wp <= '9') k = (int)wp;
            else if (wp == VK_SPACE) k = VS_KEY_SPACE;
        }
        if (k) q_push(s, VS_EV_KEY_UP, k, 0, 0);
        if (msg == WM_KEYUP) return 0;
        break;
    }
    case WM_CHAR: {
        /* A W window gets UTF-16 here, including what an IME commits. A
           character outside the BMP comes as two messages, high half first. */
        unsigned u = (unsigned)wp;
        if (!s) return 0;
        if (u >= 0xD800 && u <= 0xDBFF) { s->hi_surrogate = (WCHAR)u; return 0; }
        unsigned cp = u;
        if (u >= 0xDC00 && u <= 0xDFFF) {
            if (!s->hi_surrogate) return 0; /* stray low half */
            cp = 0x10000 + (((unsigned)s->hi_surrogate - 0xD800) << 10) + (u - 0xDC00);
        }
        s->hi_surrogate = 0;
        /* Control characters (Enter, Tab, Backspace, Ctrl+letter) are
           already reported by WM_KEYDOWN as keys. */
        if (cp < 32 || cp == 127) return 0;
        /* ASCII keeps its code in vs_key(), as on X11; anything else is
           text only (key 0). */
        QEv *e = q_push(s, VS_EV_KEY, cp < 128 ? (int)cp : 0, 0, 0);
        if (e) {
            utf8_put(cp, e->text);
            /* Text typed with AltGr reports Ctrl+Alt held; it is text, not
               a chord, and is_text_input refuses text under Ctrl or Alt. */
            if ((e->mods & (VS_MOD_CTRL | VS_MOD_ALT)) == (VS_MOD_CTRL | VS_MOD_ALT))
                e->mods &= ~(VS_MOD_CTRL | VS_MOD_ALT);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void *vs_open(const char *title, int w, int h) {
    VSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    if (headless_on()) {
        s->in.hl_index = hl_next_index++;
        headless_open_script();
        g_scale_pct = 100;
        return s;
    }
    static int registered;
    if (!registered) {
        /* The exe's own icon (resource 1, which vytoc embeds from --manifest)
           for the title bar and taskbar. Explorer reads the resource on its
           own; a window only shows it if its class names it. With no icon in
           the exe both are NULL and Windows draws its default. */
        HINSTANCE inst = GetModuleHandleW(NULL);
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = vs_wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                     LR_DEFAULTCOLOR);
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        wc.lpszClassName = L"VytoSurface";
        /* A W class makes this a Unicode window: WM_CHAR carries UTF-16 and
           the title is not filtered through the ANSI code page. */
        if (!RegisterClassExW(&wc)) { free(s); return NULL; }
        registered = 1;
    }
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    VtWBuf wt;
    vt_wbuf_from(&wt, title ? title : "");
    s->hwnd = CreateWindowExW(0, L"VytoSurface", wt.p, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    vt_wbuf_free(&wt);
    if (!s->hwnd) { free(s); return NULL; }
    s->cursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, (LONG_PTR)s);
    {
        HDC wdc = GetDC(s->hwnd);
        g_scale_pct = GetDeviceCaps(wdc, LOGPIXELSX) * 100 / 96;
        ReleaseDC(s->hwnd, wdc);
    }
    s->font = GetStockObject(ANSI_FIXED_FONT);
    make_backbuffer(s);
    ShowWindow(s->hwnd, SW_SHOW);
    UpdateWindow(s->hwnd);
    return s;
}

void vs_close(void *vs) {
    VSurf *s = vs;
    if (!s) return;
    if (s->hwnd) {
        SelectObject(s->memdc, s->bmp0);
        DeleteObject(s->bmp);
        DeleteDC(s->memdc);
        SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(s->hwnd);
    }
    if (s->icon) DestroyIcon(s->icon);
    free(s);
}

int vs_width(void *vs) { return ((VSurf *)vs)->w; }
int vs_height(void *vs) { return ((VSurf *)vs)->h; }

void vs_set_title(void *vs, const char *t) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    VtWBuf w;
    vt_wbuf_from(&w, t ? t : "");
    SetWindowTextW(s->hwnd, w.p);
    vt_wbuf_free(&w);
}

static HCURSOR cursor_for(int shape) {
    static HCURSOR cache[11];
    static const LPCWSTR ids[11] = {
        (LPCWSTR)IDC_ARROW, (LPCWSTR)IDC_IBEAM, (LPCWSTR)IDC_HAND, (LPCWSTR)IDC_WAIT,
        (LPCWSTR)IDC_CROSS, (LPCWSTR)IDC_SIZEWE, (LPCWSTR)IDC_SIZENS,
        (LPCWSTR)IDC_SIZENWSE, (LPCWSTR)IDC_SIZENESW, (LPCWSTR)IDC_SIZEALL, NULL
    };
    if (shape < 0 || shape > 10) shape = VS_CURSOR_ARROW;
    if (shape == VS_CURSOR_HIDDEN) return NULL;
    if (!cache[shape]) cache[shape] = LoadCursorW(NULL, ids[shape]);
    return cache[shape];
}

void vs_set_cursor(void *vs, int shape) {
    VSurf *s = vs;
    if (!s) return;
    if (shape < 0 || shape > VS_CURSOR_HIDDEN) shape = VS_CURSOR_ARROW;
    s->in.cursor = shape;
    if (!s->hwnd) return;
    s->cursor = cursor_for(shape);
    /* WM_SETCURSOR only comes with the next move; apply it now if the
       pointer is over our client area, so a change under a still mouse
       shows at once. */
    POINT p;
    RECT rc;
    if (GetCursorPos(&p) && WindowFromPoint(p) == s->hwnd) {
        ScreenToClient(s->hwnd, &p);
        GetClientRect(s->hwnd, &rc);
        if (PtInRect(&rc, p)) SetCursor(s->cursor);
    }
}
int vs_get_cursor(void *vs) { VSurf *s = vs; return s ? s->in.cursor : 0; }

void vs_set_ime_pos(void *vs, int x, int y, int h) {
    VSurf *s = vs;
    if (!s || !s->hwnd) return;
    HIMC imc = ImmGetContext(s->hwnd);
    if (!imc) return; /* no IME on this thread's keyboard layout */
    COMPOSITIONFORM cf;
    memset(&cf, 0, sizeof cf);
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = x;
    cf.ptCurrentPos.y = y;
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand;
    memset(&cand, 0, sizeof cand);
    cand.dwIndex = 0;
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos.x = x;
    cand.ptCurrentPos.y = y + (h > 0 ? h : 0);
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(s->hwnd, imc);
}

/* DwmSetWindowAttribute is resolved at first use, not imported: dwmapi.dll
   is absent from the Windows 7 export list this project audits against, and
   the attribute it sets only exists from Windows 10 1809. 20 is
   DWMWA_USE_IMMERSIVE_DARK_MODE; 19 is the same thing before 20H1. */
typedef HRESULT (WINAPI *VsDwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
void vs_set_dark_frame(void *vs, int on) {
    VSurf *s = vs;
    if (!s || !s->hwnd) return;
    static VsDwmSetAttr fn;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        if (m) fn = (VsDwmSetAttr)(void *)GetProcAddress(m, "DwmSetWindowAttribute");
    }
    if (!fn) return;
    BOOL v = on ? TRUE : FALSE;
    if (fn(s->hwnd, 20, &v, sizeof v) != S_OK && fn(s->hwnd, 19, &v, sizeof v) != S_OK)
        return; /* Windows 7/8/early 10: no dark frame */
    /* the frame repaints on the next activation otherwise */
    SetWindowPos(s->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* A 32-bit top-down DIB holds the pixels as-is: 0xAARRGGBB in a uint32 is
   B, G, R, A in memory, which is a DIB's layout, and an icon's alpha is
   straight like ours. The monochrome mask is all zero (opaque); with a 32-bit
   colour bitmap Windows takes transparency from the alpha channel instead. */
void vs_set_icon(void *vs, const unsigned int *argb, int w, int h) {
    VSurf *s = vs;
    if (!s || !s->hwnd || !argb || w <= 0 || h <= 0 || w > 256 || h > 256) return;
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = w;
    bi.bV5Height = -h; /* top row first */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) { if (color) DeleteObject(color); return; }
    memcpy(bits, argb, (size_t)w * (size_t)h * 4);
    size_t mrow = (size_t)((w + 15) / 16) * 2; /* monochrome rows pad to 16 bits */
    void *mbits = calloc((size_t)h, mrow);
    HBITMAP mask = mbits ? CreateBitmap(w, h, 1, 1, mbits) : NULL;
    free(mbits);
    if (!mask) { DeleteObject(color); return; }
    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!icon) return;
    SendMessageA(s->hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessageA(s->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    /* Only an icon this function made is ours to destroy; the class icon
       WM_SETICON hands back belongs to the exe. */
    if (s->icon) DestroyIcon(s->icon);
    s->icon = icon;
}


void vs_set_decorated(void *vs, int on) {
    VSurf *s = vs;
    if (!s || !s->hwnd) return;
    /* WS_POPUP is the undecorated counterpart of WS_OVERLAPPEDWINDOW. The
       SetWindowPos with SWP_FRAMECHANGED is required — without it the frame is
       not recalculated and the titlebar stays until something else resizes. */
    LONG_PTR style = on ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
                        : (WS_POPUP | WS_VISIBLE);
    SetWindowLongPtr(s->hwnd, GWL_STYLE, style);
    SetWindowPos(s->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void vs_get_position(void *vs, int *x, int *y) {
    VSurf *s = vs;
    if (x) *x = 0;
    if (y) *y = 0;
    if (!s || !s->hwnd) return;
    RECT r;
    if (GetWindowRect(s->hwnd, &r)) {
        if (x) *x = r.left;
        if (y) *y = r.top;
    }
}

void vs_move(void *vs, int x, int y) {
    VSurf *s = vs;
    if (!s || !s->hwnd) return;
    SetWindowPos(s->hwnd, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Win32's equivalent of _NET_WM_MOVERESIZE: tell DefWindowProc the click landed
   on the caption (or a border), and it runs its own modal move/resize loop with
   the system's snapping. HTCAPTION is the move case; the rest map to the eight
   edges in the same order as VS_EDGE_*. */
void vs_set_size(void *vs, int w, int h) {
    VSurf *s = vs;
    if (!s || !s->hwnd || w < 1 || h < 1) return;
    /* The caller means the client area, so grow the request by whatever frame
       the current style adds — otherwise a decorated window comes out short by
       the titlebar. */
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, (DWORD)GetWindowLongPtr(s->hwnd, GWL_STYLE), FALSE);
    SetWindowPos(s->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int vs_set_fullscreen(void *vs, int on) {
    VSurf *s = vs;
    if (!s || !s->hwnd) return 0;
    if (on) {
        /* Borderless at the rect of the monitor the window is currently on —
           the same "which monitor" rule as the X11 arm. */
        HMONITOR mon = MonitorFromWindow(s->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfo(mon, &mi)) return 0;
        SetWindowLongPtr(s->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(s->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtr(s->hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(s->hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    return 1;
}

/* Per-monitor DPI, resolved at runtime rather than linked.
 *
 * GetDpiForMonitor is Windows 8.1, and this toolchain's headers gate it behind
 * NTDDI_WINBLUE while mingw defaults NTDDI_VERSION to Server 2003 — so the
 * declaration is preprocessed away entirely and a direct call would not even
 * compile. Raising the gate would raise the floor for every other Win32 call in
 * this file, and adding Shcore to the link line would drop support for the
 * older Windows this backend was verified on. GetProcAddress avoids both, and
 * matches the dlopen/dlsym idiom the X11 arm already uses for XRandR.
 *
 * Returns 0 when the API is unavailable, which is the pre-8.1 answer. */
static int win_monitor_scale(HMONITOR mon) {
    typedef HRESULT (WINAPI *getdpi_t)(HMONITOR, int, UINT *, UINT *);
    static getdpi_t getdpi;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE h = LoadLibraryA("shcore.dll");
        if (h) getdpi = (getdpi_t)(void *)GetProcAddress(h, "GetDpiForMonitor");
    }
    if (!getdpi || !mon) return 0;
    UINT dx = 0, dy = 0;
    if (getdpi(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy) != 0) return 0;
    if (dx < 48) return 0;
    return (int)((dx * 100 + 48) / 96);
}

/* EnumDisplayMonitors hands each monitor to a callback, so collect through a
   small context rather than returning from inside it. */
typedef struct { VsMonitor *out; int max; int n; } VsMonEnum;

static BOOL CALLBACK vs_mon_cb(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp) {
    (void)dc; (void)rc;
    VsMonEnum *e = (VsMonEnum *)lp;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfo(mon, &mi)) {
        if (e->n < e->max) {
            e->out[e->n].x = mi.rcMonitor.left;
            e->out[e->n].y = mi.rcMonitor.top;
            e->out[e->n].w = mi.rcMonitor.right - mi.rcMonitor.left;
            e->out[e->n].h = mi.rcMonitor.bottom - mi.rcMonitor.top;
            e->out[e->n].primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
            /* $VYTO_SCALE for this monitor first, then what Windows says, then
               the process-wide value. */
            int sc = scale_from_env_at(e->n);
            if (!sc) sc = win_monitor_scale(mon);
            if (!sc) sc = g_scale_pct;
            e->out[e->n].scale_pct = sc > 0 ? sc : 100;
        }
        e->n++;
    }
    return TRUE;
}

int vs_capture(unsigned long win, int *out, int cap, int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    HWND hwnd = (HWND)(UINT_PTR)win;
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return 0;
    RECT r;
    if (!GetClientRect(hwnd, &r)) return 0;
    int cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw < 1 || ch < 1) return 0;
    if (w) *w = cw;
    if (h) *h = ch;
    int total = cw * ch;
    if (!out || cap <= 0) return total;

    HDC src = GetDC(hwnd);
    if (!src) return 0;
    HDC mem = CreateCompatibleDC(src);
    HBITMAP bmp = CreateCompatibleBitmap(src, cw, ch);
    HGDIOBJ old = SelectObject(mem, bmp);
    int ok = BitBlt(mem, 0, 0, cw, ch, src, 0, 0, SRCCOPY);

    int got = 0;
    if (ok) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;   /* negative: top-down rows */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        int n = total < cap ? total : cap;
        /* GetDIBits writes 0x00RRGGBB into the caller's buffer directly when the
           request is a full-width run; a partial cap just truncates the tail. */
        if (GetDIBits(mem, bmp, 0, (UINT)ch, out, &bi, DIB_RGB_COLORS)) {
            for (int i = 0; i < n; i++) out[i] &= 0xFFFFFF;
            got = total;
        }
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, src);
    return got;
}

int vs_monitors(VsMonitor *out, int max) {
    if (headless_on()) return headless_monitors(out, max);
    VsMonEnum e;
    e.out = out;
    e.max = max;
    e.n = 0;
    EnumDisplayMonitors(NULL, NULL, vs_mon_cb, (LPARAM)&e);
    if (e.n == 0) {   /* no monitor reported: describe the virtual screen */
        e.n = 1;
        if (max > 0) {
            out[0].x = 0;
            out[0].y = 0;
            out[0].w = GetSystemMetrics(SM_CXSCREEN);
            out[0].h = GetSystemMetrics(SM_CYSCREEN);
            out[0].primary = 1;
            int sc = scale_from_env_at(0);
            if (!sc) sc = g_scale_pct;
            out[0].scale_pct = sc > 0 ? sc : 100;
        }
    }
    return e.n;
}

int vs_drag_start(void *vs, int edge, int x_root, int y_root) {
    VSurf *s = vs;
    (void)x_root; (void)y_root;
    if (!s || !s->hwnd) return 0;
    if (edge < 0 || edge > 8) return 0;
    static const int ht[9] = {
        HTTOPLEFT, HTTOP, HTTOPRIGHT, HTRIGHT,
        HTBOTTOMRIGHT, HTBOTTOM, HTBOTTOMLEFT, HTLEFT, HTCAPTION
    };
    /* Release the implicit capture from the press, then re-issue the click as a
       non-client one so DefWindowProc starts its drag loop. */
    s->held = 0; /* the move loop eats the release; deliver none either */
    ReleaseCapture();
    SendMessageW(s->hwnd, WM_NCLBUTTONDOWN, (WPARAM)ht[edge], 0);
    return 1;
}

void vs_set_min_size(void *vs, int w, int h) {
    VSurf *s = vs;
    if (!s || w < 1 || h < 1) return;
    /* Stored, not applied: Win32 asks via WM_GETMINMAXINFO rather than being
       told. The values are the client area the caller wants, and the WndProc
       adjusts them to a window size there. */
    s->min_w = w;
    s->min_h = h;
}

void vs_fill_rect(void *vs, int x, int y, int w, int h, int rgb) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(cref(rgb));
    FillRect(s->memdc, &r, b);
    DeleteObject(b);
}

void vs_draw_rect(void *vs, int x, int y, int w, int h, int rgb) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(cref(rgb));
    FrameRect(s->memdc, &r, b);
    DeleteObject(b);
}

void vs_draw_line(void *vs, int x0, int y0, int x1, int y1, int rgb) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    HPEN pen = CreatePen(PS_SOLID, 1, cref(rgb));
    HGDIOBJ old = SelectObject(s->memdc, pen);
    MoveToEx(s->memdc, x0, y0, NULL);
    LineTo(s->memdc, x1, y1);
    SelectObject(s->memdc, old);
    DeleteObject(pen);
}

void vs_draw_text(void *vs, int x, int y, const char *str, int rgb) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    /* the y argument is a baseline (X11 convention); GDI wants the top */
    TEXTMETRICW tm;
    GetTextMetricsW(s->memdc, &tm);
    SetTextColor(s->memdc, cref(rgb));
    VtWBuf w;
    vt_wbuf_from(&w, str);
    TextOutW(s->memdc, x, y - tm.tmAscent, w.p, w.n);
    vt_wbuf_free(&w);
}

void vs_clip_set(void *vs, int x, int y, int w, int h) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    HRGN rgn = CreateRectRgn(x, y, x + w, y + h);
    SelectClipRgn(s->memdc, rgn);
    DeleteObject(rgn);
}

void vs_clip_clear(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    SelectClipRgn(s->memdc, NULL);
}

void vs_blit_rect(void *vs, const int *pixels, int stride_px,
                  int srcx, int srcy, int w, int h, int dstx, int dsty);

void vs_blit(void *vs, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth) {
    VSurf *s = vs;
    if (!s->hwnd || srcw <= 0 || srch <= 0) return;
    if (s->bits && dstw == srcw && dsth == srch) { /* unscaled: a plain copy */
        vs_blit_rect(vs, pixels, srcw, 0, 0, srcw, srch, dstx, dsty);
        return;
    }
    int saved = SaveDC(s->memdc); /* blits are never clipped */
    SelectClipRgn(s->memdc, NULL);
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = srcw;
    bmi.bmiHeader.biHeight = -srch; /* top-down: first row is y=0 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32; /* DWORD is 0x00RRGGBB — matches our ints */
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(s->memdc, COLORONCOLOR); /* nearest-neighbor */
    StretchDIBits(s->memdc, dstx, dsty, dstw, dsth, 0, 0, srcw, srch, pixels, &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    RestoreDC(s->memdc, saved);
}

/* unscaled sub-rect copy with explicit source stride (pixels) */
void vs_blit_rect(void *vs, const int *pixels, int stride_px,
                  int srcx, int srcy, int w, int h, int dstx, int dsty) {
    VSurf *s = vs;
    if (!s->hwnd || w <= 0 || h <= 0 || stride_px <= 0) return;
    if (s->bits) {
        /* Straight into the DIB section, row by row: no temporary, no
           StretchDIBits. Clipped to the backbuffer, since nothing else
           bounds a raw copy. GdiFlush first so a batched GDI draw cannot
           land on top of these pixels afterwards. */
        if (dstx < 0) { srcx -= dstx; w += dstx; dstx = 0; }
        if (dsty < 0) { srcy -= dsty; h += dsty; dsty = 0; }
        if (dstx + w > s->w) w = s->w - dstx;
        if (dsty + h > s->h) h = s->h - dsty;
        if (w <= 0 || h <= 0) return;
        GdiFlush();
        for (int dy = 0; dy < h; dy++)
            memcpy(s->bits + (size_t)(dsty + dy) * (size_t)s->w + dstx,
                   pixels + (size_t)(srcy + dy) * stride_px + srcx, (size_t)w * 4);
        return;
    }
    /* No DIB section (the fallback bitmap): repack the sub-rect tightly;
       SetDIBitsToDevice's source-offset rules for top-down DIBs are
       error-prone, a copy is simple and correct */
    int *tmp = malloc((size_t)w * (size_t)h * 4);
    if (!tmp) return;
    for (int dy = 0; dy < h; dy++)
        memcpy(tmp + (size_t)dy * w, pixels + (size_t)(srcy + dy) * stride_px + srcx,
               (size_t)w * 4);
    int saved = SaveDC(s->memdc); /* blits are never clipped */
    SelectClipRgn(s->memdc, NULL);
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(s->memdc, dstx, dsty, w, h, 0, 0, w, h, tmp, &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    RestoreDC(s->memdc, saved);
    free(tmp);
}

void vs_present(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    HDC dc = GetDC(s->hwnd);
    BitBlt(dc, 0, 0, s->w, s->h, s->memdc, 0, 0, SRCCOPY);
    ReleaseDC(s->hwnd, dc);
}

void vs_present_rect(void *vs, int x, int y, int w, int h) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    HDC dc = GetDC(s->hwnd);
    BitBlt(dc, x, y, w, h, s->memdc, x, y, SRCCOPY);
    ReleaseDC(s->hwnd, dc);
}

int vs_scale_pct(void *vs) {
    VSurf *s = vs;
    if (s && s->hwnd) {
        int x = 0, y = 0;
        vs_get_position(s, &x, &y);
        int idx = monitor_index_at(x, y);
        int env = scale_from_env_at(idx);
        if (env) return env;
        /* What WM_DPICHANGED last said. It outranks asking the monitor under
           the window's corner: mid-drag the corner can sit on one display
           while Windows has already moved the window to the other's DPI. */
        if (s->in.dpi_pct > 0) return s->in.dpi_pct;
        /* Ask the monitor itself. win_monitor_dpi falls back to the global when
           the platform cannot say, which is every pre-8.1 Windows. */
        VsMonitor mons[16];
        int n = vs_monitors(mons, 16);
        if (idx < n && idx < 16 && mons[idx].scale_pct > 0) return mons[idx].scale_pct;
    }
    int env = scale_from_env();
    if (env) return env;
    if (s && s->in.dpi_pct > 0) return s->in.dpi_pct; /* headless `scale N` */
    return g_scale_pct ? g_scale_pct : 100;
}

/* 1 once after the window's scale changed (WM_DPICHANGED, or a headless
   `scale N`), then 0 until it changes again. Window.run() asks on every
   RESIZE, which is how a scale change reaches the toolkit without a new
   event type: VS_EV_* is a wire contract with three other backends. */
int vs_take_scale_change(void *vs) {
    VSurf *s = vs;
    if (!s || !s->in.scale_changed) return 0;
    s->in.scale_changed = 0;
    return 1;
}

int vs_text_width(void *vs, const char *str) {
    VSurf *s = vs;
    if (!s->hwnd) return 9 * (int)strlen(str);
    SIZE sz;
    VtWBuf w;
    vt_wbuf_from(&w, str);
    if (!GetTextExtentPoint32W(s->memdc, w.p, w.n, &sz)) sz.cx = 0;
    vt_wbuf_free(&w);
    return (int)sz.cx;
}

int vs_font_ascent(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return 11;
    TEXTMETRICW tm;
    GetTextMetricsW(s->memdc, &tm);
    return (int)tm.tmAscent;
}

int vs_font_height(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return 15;
    TEXTMETRICW tm;
    GetTextMetricsW(s->memdc, &tm);
    return (int)tm.tmHeight;
}

static int q_pop(VSurf *s) {
    QEv e = s->evq[s->evq_head];
    s->evq_head = (s->evq_head + 1) % 64;
    s->evq_len--;
    s->in.key = e.key;
    s->in.x = e.x;
    s->in.y = e.y;
    s->in.mods = e.mods;
    s->in.wheel = (e.type == VS_EV_MOUSE_WHEEL) ? e.key : 0;
    s->in.wheel_x = e.wheel_x;
    s->in.button = e.button;
    memcpy(s->in.text, e.text, sizeof e.text);
    s->in.text[sizeof e.text - 1] = 0;
    return e.type;
}

/* The six input accessors, Win32 arm. NULL-guarded because raw_handle() is
   public, so a caller can hand us anything. */
int vs_key(void *vs)   { VSurf *s = vs; return s ? s->in.key : 0; }
const char *vs_text(void *vs) { VSurf *s = vs; return s ? s->in.text : ""; }
int vs_x(void *vs)     { VSurf *s = vs; return s ? s->in.x : 0; }
int vs_y(void *vs)     { VSurf *s = vs; return s ? s->in.y : 0; }
int vs_wheel(void *vs) { VSurf *s = vs; return s ? s->in.wheel : 0; }
int vs_mods(void *vs)  { VSurf *s = vs; return s ? s->in.mods : 0; }
int vs_wheel_x(void *vs) { VSurf *s = vs; return s ? s->in.wheel_x : 0; }
int vs_button(void *vs)  { VSurf *s = vs; return s ? s->in.button : 0; }

int vs_wait(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return headless_wait(&s->in, &s->w, &s->h);
    while (s->evq_len == 0) {
        MSG msg;
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) return VS_EV_CLOSE;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return q_pop(s);
}

int vs_poll(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return headless_wait(&s->in, &s->w, &s->h);
    MSG msg;
    while (s->evq_len == 0 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return s->evq_len ? q_pop(s) : VS_EV_NONE;
}

int vs_wait_timeout(void *vs, int ms) {
    VSurf *s = vs;
    if (!s->hwnd) return headless_wait(&s->in, &s->w, &s->h);
    for (;;) {
        if (s->evq_len) return q_pop(s);
        /* MWMO_INPUTAVAILABLE: also wake for input already in the queue that
           a previous PeekMessage saw — plain MsgWaitForMultipleObjects only
           signals NEW input and can stall on already-queued messages */
        DWORD r = MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)ms, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_TIMEOUT) return VS_EV_TIMER;
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

/* Waiting on window messages AND sockets at once: each fd is a socket bound to
   an event with WSAEventSelect, and MsgWaitForMultipleObjectsEx sleeps until a
   message or any of them fires. That is what lets a GUI with a channel to its
   services (vyto/desktop/channel/gui) sleep at 0% CPU instead of waking 60
   times a second.

   ws2_32 is not linked here: an app that has sockets already loaded it, so the
   three functions are looked up from the loaded module, and an app without
   sockets pays nothing. Anything that is not a socket (WSAEventSelect fails),
   or no ws2_32 at all, degrades for that call to a bounded wait of at most
   16 ms -- the caller asked for a longer one only because it believed the fds
   would wake it, so a fallback must not stretch its latency to that.

   WSAEventSelect leaves the socket non-blocking, which every poll source is
   already; the association is removed again after each wait. */
#define VS_FD_READ    0x01
#define VS_FD_ACCEPT  0x08
#define VS_FD_CLOSE   0x20
/* Not FD_CONNECT: re-arming it on an already-connected socket on every wait
   signals the event at once, and the loop spun at ~85k wakeups a second
   (measured under Wine). A connect in progress is the poll source's own
   business (vyto/desktop/channel polls it); the loop's own timer ceiling
   bounds how late it is noticed. FD_WRITE is not watched either: poll
   sources only ever wait to read. */

typedef HANDLE (WINAPI *vs_wsa_create_fn)(void);
typedef int (WINAPI *vs_wsa_select_fn)(UINT_PTR, HANDLE, long);
typedef BOOL (WINAPI *vs_wsa_close_fn)(HANDLE);

int vs_wait_timeout_fds(void *vs, int ms, const int *fds, int n) {
    VSurf *s = vs;
    if (!s->hwnd || n <= 0) return vs_wait_timeout(vs, ms);
    if (s->evq_len) return q_pop(s);
    int fallback = ms < 0 || ms > 16 ? 16 : ms;
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws) return vs_wait_timeout(vs, fallback);
    vs_wsa_create_fn wcreate = (vs_wsa_create_fn)(void (*)(void))GetProcAddress(ws, "WSACreateEvent");
    vs_wsa_select_fn wselect = (vs_wsa_select_fn)(void (*)(void))GetProcAddress(ws, "WSAEventSelect");
    vs_wsa_close_fn wclose = (vs_wsa_close_fn)(void (*)(void))GetProcAddress(ws, "WSACloseEvent");
    if (!wcreate || !wselect || !wclose) return vs_wait_timeout(vs, fallback);
    HANDLE evs[MAXIMUM_WAIT_OBJECTS - 1];
    int k = 0;
    int ok = 1;
    for (int i = 0; i < n && k < MAXIMUM_WAIT_OBJECTS - 1; i++) {
        HANDLE ev = wcreate();
        if (ev == NULL || ev == INVALID_HANDLE_VALUE) { ok = 0; break; }
        if (wselect((UINT_PTR)(unsigned)fds[i], ev,
                    VS_FD_READ | VS_FD_ACCEPT | VS_FD_CLOSE) != 0) {
            wclose(ev);
            ok = 0;
            break;
        }
        evs[k++] = ev;
    }
    DWORD r = WAIT_TIMEOUT;
    if (ok) {
        r = MsgWaitForMultipleObjectsEx((DWORD)k, evs, ms < 0 ? INFINITE : (DWORD)ms, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
    }
    for (int i = 0; i < k; i++) {
        wselect((UINT_PTR)(unsigned)fds[i], NULL, 0);
        wclose(evs[i]);
    }
    if (!ok) return vs_wait_timeout(vs, fallback);
    if (r == WAIT_TIMEOUT || (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + (DWORD)k))
        return VS_EV_TIMER;   /* a socket is ready: the caller pumps its poll sources */
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return s->evq_len ? q_pop(s) : VS_EV_TIMER;
}

/* Win32 has no waitable descriptor for window events: the loop blocks in
   MsgWaitForMultipleObjectsEx on a thread message queue, which is a kernel
   object of a different kind and cannot be handed to select/poll/epoll.
   Reporting -1 rather than faking one is what keeps the caller honest — it
   falls back to a bounded vs_wait_timeout, the same thing
   Window.add_poll_source does today.

   (A real integration here would invert the loop: hand the reactor's next
   timeout to MsgWaitForMultipleObjectsEx and let IT be the blocking wait, with
   the socket set polled at zero timeout after. That is a Win32-shaped design,
   not an fd, which is precisely why this returns -1.) */
int vs_event_fd(void *vs) { (void)vs; return -1; }

/* No damage region: WM_PAINT's update rect is not plumbed through the event
   ring, so a repaint here is always full. Returning 0 is the documented
   "repaint everything" answer, not a stub with a wrong value. */
int vs_damage(void *vs, VsRect *out) {
    (void)vs; (void)out;
    return 0;
}

/* Messages already decoded into the queue by a previous PeekMessage. */
int vs_events_pending(void *vs) { VSurf *s = vs; return s ? s->evq_len : 0; }

/* Sockets, yes (vs_wait_timeout_fds above); anything else degrades per call. */
int vs_can_wait_fds(void *vs) { VSurf *s = vs; return s && s->hwnd ? 1 : 0; }

void *vs_native_display(void *vs) {
    (void)vs;
    return NULL; /* no display concept on Win32 */
}
unsigned long vs_native_window(void *vs) { return (unsigned long)(uintptr_t)((VSurf *)vs)->hwnd; }
void *vs_native_gc(void *vs) { return ((VSurf *)vs)->memdc; }

void vs_clipboard_set(void *vs, const char *text) {
    VSurf *s = vs;
    if (!text) text = "";
    if (!s->hwnd) { clip_store(&clip_local, text); return; }
    int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wn <= 0) return;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wn * sizeof(WCHAR));
    if (!hg) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, GlobalLock(hg), wn);
    GlobalUnlock(hg);
    if (OpenClipboard(s->hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hg); /* the system owns hg now */
        CloseClipboard();
    } else {
        GlobalFree(hg);
    }
}

const char *vs_clipboard_get(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return clip_local ? clip_local : "";
    static char *buf; /* grows; valid until the next call */
    const char *ret = "";
    if (!OpenClipboard(s->hwnd)) return "";
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR *w = GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (n > 0) {
                char *nb = realloc(buf, (size_t)n);
                if (nb) {
                    buf = nb;
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL);
                    ret = buf;
                }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ret;
}

#else
/* ================================================================== X11 */

#include <dlfcn.h>   /* XRandR is dlopen'd, not linked — see vs_monitors */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <locale.h>
#include <sys/select.h>

#ifdef __linux__ /* fbdev sub-backend (VS_FBDEV=/dev/fb0) */
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#endif

typedef struct VSurf {
    Display *dpy; /* NULL in headless and fbdev modes */
    Window win;
    Pixmap back;
    GC gc;         /* draw GC — vs_clip_set applies here */
    GC pgc;        /* blit/present GC — never clipped */
    int w, h;
    Atom wm_delete;
    XIM im;        /* input method (UTF-8 text input); may be NULL */
    XIC ic;
    XImage *img;   /* cached blit staging image, resized on demand */
    int img_w, img_h;
    /* union of the damage rects of the Expose run just delivered, so a caller
       can repaint only what the server actually lost. dmg_w == 0 means "no
       region reported" — repaint everything. */
    int dmg_x, dmg_y, dmg_w, dmg_h;
    /* clipboard (CLIPBOARD selection) */
    Atom a_clipboard, a_utf8, a_targets, a_incr, a_clip_prop;
    char *clip_got; /* last text fetched from another owner */
    Cursor cursors[11]; /* per VS_CURSOR_*, created on first use; 0 = none yet */
    /* fbdev mode: fb_on != 0, everything above except w/h unused */
    int fb_on;
    int fb_fd;
    uint8_t *fb_mem;   /* mmapped framebuffer (or file) */
    long fb_maplen;
    long fb_stride;    /* bytes per row */
    int fb_bpp;        /* 16 or 32 */
    int fb_ro, fb_rl, fb_go, fb_gl, fb_bo, fb_bl; /* channel offset/length */
    uint32_t *bb;      /* software backbuffer, w*h of 0x00RRGGBB */
    int fb_clip_on, fb_cx0, fb_cy0, fb_cx1, fb_cy1;
    int fb_expose;     /* deliver one EV_EXPOSE on the first wait */

    VsInput in;   /* this window's input state; see VsInput above */
} VSurf;

static XFontStruct *font;       /* one bitmap UI font, shared, never freed */

/* ---- color mapping: 0xRRGGBB -> X pixel ---- */

static int truecolor; /* default visual is 24/32-bit TrueColor with 8/8/8 masks */

static unsigned long pixel_of(Display *dpy, int rgb) {
    if (truecolor) return (unsigned long)(rgb & 0xFFFFFF);
    static int cached_rgb[64];
    static unsigned long cached_px[64];
    static int cache_used[64];
    int slot = (rgb ^ (rgb >> 12)) & 63;
    if (cache_used[slot] && cached_rgb[slot] == rgb) return cached_px[slot];
    XColor c;
    c.red = (unsigned short)(((rgb >> 16) & 0xFF) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 0xFF) * 257);
    c.blue = (unsigned short)((rgb & 0xFF) * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c))
        c.pixel = BlackPixel(dpy, DefaultScreen(dpy));
    cached_rgb[slot] = rgb;
    cached_px[slot] = c.pixel;
    cache_used[slot] = 1;
    return c.pixel;
}

/* Xft.dpi from the X resource database, else the physical screen DPI;
   returned as a percent of the 96dpi baseline. */
/* Scale from a monitor's pixel width and physical width in mm, or 0 when the
   monitor did not report a usable physical size (projectors, KVMs and virtual
   outputs often report 0). */
static int dpi_scale_pct(int px, int mm) {
    if (px < 1 || mm < 1) return 0;
    double dpi = (double)px * 25.4 / (double)mm;
    if (dpi < 48.0 || dpi > 480.0) return 0;
    return (int)(dpi * 100.0 / 96.0 + 0.5);
}

/* The desktop's declared scale, from the one global Xft.dpi resource. X has no
   per-output equivalent, so this is the same answer for every monitor. */
static int x11_xft_scale(Display *dpy) {
    const char *rms = XResourceManagerString(dpy);
    if (rms) {
        const char *p = strstr(rms, "Xft.dpi:");
        if (p) {
            double dpi = atof(p + 8);
            if (dpi >= 48.0) return (int)(dpi * 100.0 / 96.0 + 0.5);
        }
    }
    return 0;
}

static int x11_detect_scale(Display *dpy) {
    int xft = x11_xft_scale(dpy);
    if (xft) return xft;
    int scr = DefaultScreen(dpy);
    int sc = dpi_scale_pct(DisplayWidth(dpy, scr), DisplayWidthMM(dpy, scr));
    return sc > 0 ? sc : 100;
}

/* =================================================== fbdev sub-backend
 * Selected at runtime by VS_FBDEV=<path> (usually /dev/fb0). No display
 * server: drawing runs a software rasterizer into a 0x00RRGGBB backbuffer,
 * vs_present converts rows into the mmapped framebuffer (32bpp or 16bpp,
 * channel offsets honored), input comes from evdev. When the path is a
 * regular file (ioctl fails) it becomes a render-to-file target sized by
 * VS_FB_W/VS_FB_H — the test path, with no console or input plumbing. */
#ifdef __linux__

/* 8x8 bitmap font, printable ASCII 0x20-0x7E; row per byte, bit0 = leftmost
   pixel (the public-domain "font8x8" glyphs) */
static const uint8_t fb_font[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

#define FB_FONT_W 8
#define FB_FONT_ASCENT 6 /* baseline sits above the two descender rows */
#define FB_FONT_H 9      /* 8 glyph rows + 1 of leading */

/* US-layout scancode -> char maps for the classic PC codes 0..57
   (KEY_ESC=1 .. KEY_SPACE=57); special keys are handled by code first */
static const char fb_km[58] =
    "\0\0331234567890-=\10\11qwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char fb_km_sh[58] =
    "\0\033!@#$%^&*()_+\10\11QWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

/* evdev input state (single window, like the other backends' statics) */
static int fb_in[16];
static int fb_nin;
static int fb_mx, fb_my;    /* pointer position, backend-tracked */
static int fb_mods_state;
static volatile sig_atomic_t fb_close_req;

static void fb_on_sig(int sig) {
    (void)sig;
    fb_close_req = 1;
}

/* event ring, mirroring the Win32 queue shape */
typedef struct FbEv {
    int type, key, x, y, wheel, mods;
    int wheel_x, button;
    char text[8];
} FbEv;
static FbEv fbq[64];
static int fbq_head, fbq_len;

static void fbq_push(int type, int key, int wheel, const char *text) {
    if (type == VS_EV_MOUSE_MOVE && fbq_len > 0) {
        FbEv *tail = &fbq[(fbq_head + fbq_len - 1) % 64];
        if (tail->type == VS_EV_MOUSE_MOVE) { /* coalesce move bursts */
            tail->x = fb_mx;
            tail->y = fb_my;
            tail->mods = fb_mods_state;
            return;
        }
    }
    if (fbq_len == 64) return;
    FbEv *e = &fbq[(fbq_head + fbq_len) % 64];
    e->type = type;
    e->key = key;
    e->x = fb_mx;
    e->y = fb_my;
    e->wheel = wheel;
    e->wheel_x = 0;
    e->button = 0;
    e->mods = fb_mods_state;
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    fbq_len++;
}

/* the event fbq_push just queued, to fill the fields only some events carry */
static FbEv *fbq_last(void) {
    return fbq_len ? &fbq[(fbq_head + fbq_len - 1) % 64] : NULL;
}

static int fbq_pop(VSurf *s) {
    FbEv e = fbq[fbq_head];
    fbq_head = (fbq_head + 1) % 64;
    fbq_len--;
    s->in.key = e.key;
    s->in.x = e.x;
    s->in.y = e.y;
    s->in.wheel = e.wheel;
    s->in.wheel_x = e.wheel_x;
    s->in.button = e.button;
    s->in.mods = e.mods;
    snprintf(s->in.text, sizeof s->in.text, "%s", e.text);
    return e.type;
}

static void fb_handle(VSurf *s, const struct input_event *ev) {
    if (ev->type == EV_REL) {
        if (ev->code == REL_X || ev->code == REL_Y) {
            if (ev->code == REL_X) fb_mx += ev->value;
            else fb_my += ev->value;
            if (fb_mx < 0) fb_mx = 0;
            if (fb_my < 0) fb_my = 0;
            if (fb_mx >= s->w) fb_mx = s->w - 1;
            if (fb_my >= s->h) fb_my = s->h - 1;
            fbq_push(VS_EV_MOUSE_MOVE, 0, 0, "");
        } else if (ev->code == REL_WHEEL && ev->value != 0) {
            /* evdev: +1 notch = wheel up; match X11's up = -3 lines */
            fbq_push(VS_EV_MOUSE_WHEEL, 0, -3 * ev->value, "");
        } else if (ev->code == REL_HWHEEL && ev->value != 0) {
            /* evdev: +1 = tilt right, which is already positive = right */
            int before = fbq_len;
            fbq_push(VS_EV_MOUSE_WHEEL, 0, 0, "");
            if (fbq_len > before) fbq_last()->wheel_x = 3 * ev->value;
        }
        return;
    }
    if (ev->type != EV_KEY) return;
    int code = ev->code, val = ev->value; /* 0 = up, 1 = down, 2 = repeat */
    if (code == BTN_LEFT) {
        if (val == 1) fbq_push(VS_EV_MOUSE_DOWN, 0, 0, "");
        else if (val == 0) fbq_push(VS_EV_MOUSE_UP, 0, 0, "");
        return;
    }
    if (code == BTN_RIGHT) {
        if (val == 1) fbq_push(VS_EV_MOUSE_RDOWN, 0, 0, "");
        else if (val == 0) {
            int before = fbq_len;
            fbq_push(VS_EV_BUTTON_UP, 0, 0, "");
            if (fbq_len > before) fbq_last()->button = VS_BUTTON_RIGHT;
        }
        return;
    }
    if (code == BTN_MIDDLE || code == BTN_SIDE || code == BTN_EXTRA) {
        if (val != 0 && val != 1) return; /* no autorepeat for buttons */
        int before = fbq_len;
        fbq_push(val ? VS_EV_BUTTON_DOWN : VS_EV_BUTTON_UP, 0, 0, "");
        if (fbq_len > before)
            fbq_last()->button = code == BTN_MIDDLE ? VS_BUTTON_MIDDLE
                               : code == BTN_SIDE ? VS_BUTTON_BACK : VS_BUTTON_FORWARD;
        return;
    }
    if (code >= BTN_MISC) return; /* other buttons/axes */
    int mod = 0, vk = 0;
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: mod = VS_MOD_SHIFT; vk = VS_KEY_SHIFT; break;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: mod = VS_MOD_CTRL; vk = VS_KEY_CTRL; break;
    case KEY_LEFTALT: case KEY_RIGHTALT: mod = VS_MOD_ALT; vk = VS_KEY_ALT; break;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: mod = VS_MOD_SUPER; vk = VS_KEY_SUPER; break;
    default: break;
    }
    if (mod) { /* modifiers: track state and deliver as their own events */
        if (val == 1) fb_mods_state |= mod;
        else if (val == 0) fb_mods_state &= ~mod;
        if (val != 2) fbq_push(val ? VS_EV_KEY : VS_EV_KEY_UP, vk, 0, "");
        return;
    }
    char text[2] = "";
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: vk = VS_KEY_ENTER; break;
    case KEY_BACKSPACE: vk = VS_KEY_BACKSPACE; break;
    case KEY_ESC: vk = VS_KEY_ESC; break;
    case KEY_UP: vk = VS_KEY_UP; break;
    case KEY_DOWN: vk = VS_KEY_DOWN; break;
    case KEY_LEFT: vk = VS_KEY_LEFT; break;
    case KEY_RIGHT: vk = VS_KEY_RIGHT; break;
    case KEY_DELETE: vk = VS_KEY_DELETE; break;
    case KEY_TAB: vk = VS_KEY_TAB; break;
    case KEY_HOME: vk = VS_KEY_HOME; break;
    case KEY_END: vk = VS_KEY_END; break;
    case KEY_PAGEUP: vk = VS_KEY_PAGEUP; break;
    case KEY_PAGEDOWN: vk = VS_KEY_PAGEDOWN; break;
    case KEY_INSERT: vk = VS_KEY_INSERT; break;
    case KEY_F11: vk = VS_KEY_F11; break;
    case KEY_F12: vk = VS_KEY_F12; break;
    default:
        if (code >= KEY_F1 && code <= KEY_F10) vk = VS_KEY_F1 + (code - KEY_F1);
        break;
    }
    if (!vk && code > 0 && code < 58) {
        char ch = (fb_mods_state & VS_MOD_SHIFT) ? fb_km_sh[code] : fb_km[code];
        if ((unsigned char)ch >= 32) {
            vk = (unsigned char)ch;
            /* Ctrl/Alt chords deliver the key but no insertable text,
               matching the X11 and Win32 backends */
            if (!(fb_mods_state & (VS_MOD_CTRL | VS_MOD_ALT))) {
                text[0] = ch;
                text[1] = 0;
            }
        }
    }
    if (!vk) return;
    if (val != 0) fbq_push(VS_EV_KEY, vk, 0, text);
    else fbq_push(VS_EV_KEY_UP, vk, 0, "");
}

static void fb_pump(VSurf *s) {
    struct input_event ev[64];
    for (int i = 0; i < fb_nin; i++) {
        for (;;) {
            ssize_t n = read(fb_in[i], ev, sizeof ev);
            if (n < (ssize_t)sizeof ev[0]) break;
            int cnt = (int)(n / (ssize_t)sizeof ev[0]);
            for (int j = 0; j < cnt; j++) fb_handle(s, &ev[j]);
        }
    }
}

/* block <= ms for the next event (ms < 0: forever; block == 0: poll).
   Returns VS_EV_NONE on an empty poll, VS_EV_TIMER on timeout. */
static int fb_next(VSurf *s, int block, int ms) {
    if (s->fb_expose) { /* the "window is up" event X11 apps expect */
        s->fb_expose = 0;
        return VS_EV_EXPOSE;
    }
    for (;;) {
        if (fb_close_req) return VS_EV_CLOSE;
        if (fbq_len) return fbq_pop(s);
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        for (int i = 0; i < fb_nin; i++) {
            FD_SET(fb_in[i], &rf);
            if (fb_in[i] > maxfd) maxfd = fb_in[i];
        }
        struct timeval tv, *ptv = NULL;
        if (!block) {
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            ptv = &tv;
        } else if (ms >= 0) {
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            ptv = &tv;
        } else if (maxfd < 0) {
            /* no input devices and asked to block forever: nap in slices so
               a SIGINT-driven close request still gets noticed */
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;
            ptv = &tv;
        }
        int rc = select(maxfd + 1, maxfd >= 0 ? &rf : NULL, NULL, NULL, ptv);
        if (rc < 0) continue; /* EINTR: re-check fb_close_req */
        if (rc == 0) {
            if (!block) return VS_EV_NONE;
            if (ms >= 0) return VS_EV_TIMER;
            continue;
        }
        fb_pump(s);
    }
}

static void fb_scan_input(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && fb_nin < 16) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long evbits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits) < 0 ||
            !(evbits & ((1UL << EV_KEY) | (1UL << EV_REL)))) {
            close(fd);
            continue;
        }
        fb_in[fb_nin++] = fd;
    }
    closedir(d);
}

/* best-effort console takeover: raw keyboard-less tty (no echo, no canonical
   mode; ISIG stays on so Ctrl+C still requests a close) and KD_GRAPHICS so
   the console cursor/printk stop scribbling over the framebuffer */
static struct termios fb_tio_saved;
static int fb_tio_on;
static int fb_kd_saved = -1;

static void fb_tty_setup(void) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &fb_tio_saved) == 0) {
        struct termios t = fb_tio_saved;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) fb_tio_on = 1;
        int mode = 0;
        if (ioctl(STDIN_FILENO, KDGETMODE, &mode) == 0) {
            fb_kd_saved = mode;
            ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS);
        }
    }
}

static void fb_tty_restore(void) {
    if (fb_kd_saved >= 0) {
        ioctl(STDIN_FILENO, KDSETMODE, fb_kd_saved);
        fb_kd_saved = -1;
    }
    if (fb_tio_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &fb_tio_saved);
        fb_tio_on = 0;
    }
}

static int fb_env_int(const char *name, int def) {
    const char *e = getenv(name);
    int v = (e && *e) ? atoi(e) : 0;
    return v > 0 ? v : def;
}

static int fb_setup(VSurf *s, const char *path) {
    s->fb_fd = open(path, O_RDWR | O_CREAT, 0666);
    if (s->fb_fd < 0) return 0;
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    int real_fb = ioctl(s->fb_fd, FBIOGET_VSCREENINFO, &vi) == 0 &&
                  ioctl(s->fb_fd, FBIOGET_FSCREENINFO, &fi) == 0;
    if (real_fb) {
        s->w = (int)vi.xres;
        s->h = (int)vi.yres;
        s->fb_stride = (long)fi.line_length;
        s->fb_bpp = (int)vi.bits_per_pixel;
        s->fb_ro = (int)vi.red.offset;
        s->fb_rl = (int)vi.red.length;
        s->fb_go = (int)vi.green.offset;
        s->fb_gl = (int)vi.green.length;
        s->fb_bo = (int)vi.blue.offset;
        s->fb_bl = (int)vi.blue.length;
        s->fb_maplen = (long)fi.smem_len;
    } else {
        /* regular-file target: VS_FB_W/VS_FB_H, 32bpp XRGB, tight stride */
        s->w = fb_env_int("VS_FB_W", 640);
        s->h = fb_env_int("VS_FB_H", 480);
        s->fb_stride = (long)s->w * 4;
        s->fb_bpp = 32;
        s->fb_ro = 16;
        s->fb_go = 8;
        s->fb_bo = 0;
        s->fb_rl = s->fb_gl = s->fb_bl = 8;
        s->fb_maplen = s->fb_stride * s->h;
        if (ftruncate(s->fb_fd, (off_t)s->fb_maplen) != 0) {
            close(s->fb_fd);
            return 0;
        }
    }
    if ((s->fb_bpp != 16 && s->fb_bpp != 32) || s->fb_rl < 1 || s->fb_rl > 8 ||
        s->fb_gl < 1 || s->fb_gl > 8 || s->fb_bl < 1 || s->fb_bl > 8 ||
        s->w < 1 || s->h < 1 || s->fb_maplen < s->fb_stride * s->h) {
        close(s->fb_fd);
        return 0; /* exotic layouts (planar, palette, >8-bit channels): no */
    }
    s->fb_mem = mmap(NULL, (size_t)s->fb_maplen, PROT_READ | PROT_WRITE,
                     MAP_SHARED, s->fb_fd, 0);
    if (s->fb_mem == MAP_FAILED) {
        s->fb_mem = NULL;
        close(s->fb_fd);
        return 0;
    }
    s->bb = calloc((size_t)s->w * (size_t)s->h, 4);
    if (!s->bb) {
        munmap(s->fb_mem, (size_t)s->fb_maplen);
        s->fb_mem = NULL;
        close(s->fb_fd);
        return 0;
    }
    if (real_fb) { /* console + input plumbing only on a real framebuffer */
        fb_scan_input();
        fb_tty_setup();
        signal(SIGINT, fb_on_sig);
        signal(SIGTERM, fb_on_sig);
    }
    fb_mx = s->w / 2;
    fb_my = s->h / 2;
    s->fb_expose = 1;
    s->fb_on = 1;
    return 1;
}

static void fb_close(VSurf *s) {
    fb_tty_restore();
    for (int i = 0; i < fb_nin; i++) close(fb_in[i]);
    fb_nin = 0;
    if (s->fb_mem) munmap(s->fb_mem, (size_t)s->fb_maplen);
    if (s->fb_fd >= 0) close(s->fb_fd);
    free(s->bb);
    s->bb = NULL;
    s->fb_on = 0;
}

/* ---- software rasterizer into the 0x00RRGGBB backbuffer ---- */

static void fb_px(VSurf *s, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if (s->fb_clip_on &&
        (x < s->fb_cx0 || y < s->fb_cy0 || x >= s->fb_cx1 || y >= s->fb_cy1))
        return;
    s->bb[(long)y * s->w + x] = c;
}

static void fb_fill(VSurf *s, int x, int y, int w, int h, int rgb) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (s->fb_clip_on) {
        if (x0 < s->fb_cx0) x0 = s->fb_cx0;
        if (y0 < s->fb_cy0) y0 = s->fb_cy0;
        if (x1 > s->fb_cx1) x1 = s->fb_cx1;
        if (y1 > s->fb_cy1) y1 = s->fb_cy1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    uint32_t c = (uint32_t)(rgb & 0xFFFFFF);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = s->bb + (long)yy * s->w;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void fb_rect(VSurf *s, int x, int y, int w, int h, int rgb) {
    if (w <= 0 || h <= 0) return;
    fb_fill(s, x, y, w, 1, rgb);
    fb_fill(s, x, y + h - 1, w, 1, rgb);
    fb_fill(s, x, y, 1, h, rgb);
    fb_fill(s, x + w - 1, y, 1, h, rgb);
}

static void fb_line(VSurf *s, int x0, int y0, int x1, int y1, int rgb) {
    uint32_t c = (uint32_t)(rgb & 0xFFFFFF);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        fb_px(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void fb_text(VSurf *s, int x, int y, const char *str, int rgb) {
    uint32_t c = (uint32_t)(rgb & 0xFFFFFF);
    int top = y - FB_FONT_ASCENT; /* y is the baseline (X11 convention) */
    for (; *str; str++, x += FB_FONT_W) {
        unsigned ch = (unsigned char)*str;
        if (ch >= 0x80) {
            if ((ch & 0xC0) == 0x80) { /* UTF-8 continuation: no cell */
                x -= FB_FONT_W;
                continue;
            }
            ch = '?'; /* non-ASCII leads render as a placeholder cell */
        } else if (ch < 0x20 || ch > 0x7E) {
            continue;
        }
        const uint8_t *g = fb_font[ch - 0x20];
        for (int ry = 0; ry < 8; ry++) {
            uint8_t bits = g[ry];
            for (int rx = 0; bits; rx++, bits >>= 1)
                if (bits & 1) fb_px(s, x + rx, top + ry, c);
        }
    }
}

/* blits are never clipped (matches the X11/GDI arms) */
static void fb_blit(VSurf *s, const int *pixels, int srcw, int srch,
                    int dstx, int dsty, int dstw, int dsth) {
    for (int dy = 0; dy < dsth; dy++) {
        int ty = dsty + dy;
        if (ty < 0 || ty >= s->h) continue;
        const int *src = pixels + (long)(dy * srch / dsth) * srcw;
        uint32_t *row = s->bb + (long)ty * s->w;
        for (int dx = 0; dx < dstw; dx++) {
            int tx = dstx + dx;
            if (tx < 0 || tx >= s->w) continue;
            int sx = srcw == dstw ? dx : dx * srcw / dstw;
            row[tx] = (uint32_t)(src[sx] & 0xFFFFFF);
        }
    }
}

static void fb_blit_sub(VSurf *s, const int *pixels, int stride_px, int srcx,
                        int srcy, int w, int h, int dstx, int dsty) {
    for (int dy = 0; dy < h; dy++) {
        int ty = dsty + dy;
        if (ty < 0 || ty >= s->h) continue;
        const int *src = pixels + (long)(srcy + dy) * stride_px + srcx;
        uint32_t *row = s->bb + (long)ty * s->w;
        for (int dx = 0; dx < w; dx++) {
            int tx = dstx + dx;
            if (tx < 0 || tx >= s->w) continue;
            row[tx] = (uint32_t)(src[dx] & 0xFFFFFF);
        }
    }
}

/* copy backbuffer rows into the mmapped framebuffer, honoring the fb's
   stride and channel layout; the vs_present of this backend */
static void fb_flush(VSurf *s, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || !s->fb_mem) return;
    int bytes = s->fb_bpp / 8;
    int direct = s->fb_bpp == 32 && s->fb_ro == 16 && s->fb_go == 8 &&
                 s->fb_bo == 0; /* XRGB8888: rows match the backbuffer */
    for (int dy = 0; dy < h; dy++) {
        const uint32_t *src = s->bb + (long)(y + dy) * s->w + x;
        uint8_t *dst = s->fb_mem + (long)(y + dy) * s->fb_stride + (long)x * bytes;
        if (direct) {
            memcpy(dst, src, (size_t)w * 4);
            continue;
        }
        for (int dx = 0; dx < w; dx++) {
            uint32_t c = src[dx];
            uint32_t v = ((((c >> 16) & 0xFF) >> (8 - s->fb_rl)) << s->fb_ro) |
                         ((((c >> 8) & 0xFF) >> (8 - s->fb_gl)) << s->fb_go) |
                         (((c & 0xFF) >> (8 - s->fb_bl)) << s->fb_bo);
            if (bytes == 4) ((uint32_t *)(void *)dst)[dx] = v;
            else ((uint16_t *)(void *)dst)[dx] = (uint16_t)v;
        }
    }
}

#else /* !__linux__: X11-only platforms never enter fbdev mode */

static int fb_setup(VSurf *s, const char *path) { (void)s; (void)path; return 0; }
static void fb_close(VSurf *s) { (void)s; }
static void fb_fill(VSurf *s, int x, int y, int w, int h, int rgb) {
    (void)s; (void)x; (void)y; (void)w; (void)h; (void)rgb;
}
static void fb_rect(VSurf *s, int x, int y, int w, int h, int rgb) {
    (void)s; (void)x; (void)y; (void)w; (void)h; (void)rgb;
}
static void fb_line(VSurf *s, int x0, int y0, int x1, int y1, int rgb) {
    (void)s; (void)x0; (void)y0; (void)x1; (void)y1; (void)rgb;
}
static void fb_text(VSurf *s, int x, int y, const char *str, int rgb) {
    (void)s; (void)x; (void)y; (void)str; (void)rgb;
}
static void fb_blit(VSurf *s, const int *p, int sw, int sh, int dx, int dy,
                    int dw, int dh) {
    (void)s; (void)p; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
}
static void fb_blit_sub(VSurf *s, const int *p, int st, int sx, int sy, int w,
                        int h, int dx, int dy) {
    (void)s; (void)p; (void)st; (void)sx; (void)sy; (void)w; (void)h; (void)dx; (void)dy;
}
static void fb_flush(VSurf *s, int x, int y, int w, int h) {
    (void)s; (void)x; (void)y; (void)w; (void)h;
}
static int fb_next(VSurf *s, int block, int ms) {
    (void)s; (void)block; (void)ms;
    return VS_EV_CLOSE;
}
#define FB_FONT_W 8
#define FB_FONT_ASCENT 6
#define FB_FONT_H 9

#endif /* __linux__ */

void *vs_open(const char *title, int w, int h) {
    VSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    if (headless_on()) {
        s->in.hl_index = hl_next_index++;
        headless_open_script();
        g_scale_pct = 100;
        return s;
    }
    const char *fbdev = getenv("VS_FBDEV");
    if (fbdev && *fbdev) { /* framebuffer mode: no display server */
        if (!fb_setup(s, fbdev)) {
            free(s);
            return NULL;
        }
        g_scale_pct = 100; /* $VYTO_SCALE still overrides via scale_from_env */
        return s;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { free(s); return NULL; }
    int scr = DefaultScreen(dpy);
    Visual *vis = DefaultVisual(dpy, scr);
    int depth = DefaultDepth(dpy, scr);
    truecolor = vis->class == TrueColor && (depth == 24 || depth == 32) &&
                vis->red_mask == 0xFF0000 && vis->green_mask == 0x00FF00 &&
                vis->blue_mask == 0x0000FF;
    g_scale_pct = x11_detect_scale(dpy);
    s->dpy = dpy;
    /* Two attributes here are what keep a resize from flashing white, and both
       are about the *server's* behaviour before our repaint gets a chance to
       run — not about anything we draw.

       background_pixmap None: with a background pixel set (XCreateSimpleWindow
       forces one, and White is the conventional pick) the server paints it into
       any newly exposed area before delivering Expose. Asking for None means it
       paints nothing and the old pixels simply stay until we present.

       bit_gravity NorthWestGravity: the default is ForgetGravity, under which a
       resize discards the window's whole contents and tiles it with the
       background — the entire window, not just the new strip. NorthWest keeps
       the top-left content anchored, so a grow shows the previous frame in the
       old region while only the new edge is undefined. */
    XSetWindowAttributes swa;
    swa.background_pixmap = None;
    swa.border_pixel = BlackPixel(dpy, scr);
    swa.bit_gravity = NorthWestGravity;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
    s->win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)w, (unsigned)h, 1,
                           depth, InputOutput, vis,
                           CWBackPixmap | CWBorderPixel | CWBitGravity | CWEventMask, &swa);
    XStoreName(dpy, s->win, title);
    /* the event mask went in through CWEventMask above; no XSelectInput needed */
    /* only emit KeyRelease on real release, not auto-repeat — matters for
       held-key game input (EV_KEY / EV_KEY_UP pairing) */
    XkbSetDetectableAutoRepeat(dpy, True, NULL);
    s->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, s->win, &s->wm_delete, 1);
    s->a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    s->a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    s->a_targets = XInternAtom(dpy, "TARGETS", False);
    s->a_incr = XInternAtom(dpy, "INCR", False);
    s->a_clip_prop = XInternAtom(dpy, "VS_CLIP", False);
    s->gc = XCreateGC(dpy, s->win, 0, 0);
    s->pgc = XCreateGC(dpy, s->win, 0, 0);
    /* Prefer an iso10646-1 (Unicode/16-bit) core font so XDrawString16 can
       render glyphs beyond Latin-1 (⌫ ± π √ …). Fall back to the 8-bit
       9x15/fixed fonts — XDrawString16 still draws their low range correctly. */
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso10646-1");
    if (!font) font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--15-*-*-*-*-*-iso10646-1");
    if (!font) font = XLoadQueryFont(dpy, "-*-*-medium-r-normal--15-*-*-*-*-*-iso10646-1");
    if (!font) font = XLoadQueryFont(dpy, "-*-*-medium-r-normal--*-*-*-*-*-*-iso10646-1");
    if (!font) font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, s->gc, font->fid);
    s->back = XCreatePixmap(dpy, s->win, (unsigned)w, (unsigned)h, (unsigned)depth);
    /* input method for UTF-8 text; falls back to Latin-1 XLookupString */
    setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    s->im = XOpenIM(dpy, NULL, NULL, NULL);
    if (s->im) {
        s->ic = XCreateIC(s->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, s->win, XNFocusWindow, s->win, NULL);
        if (!s->ic) { XCloseIM(s->im); s->im = NULL; }
    }
    XMapWindow(dpy, s->win);
    return s;
}

static void fb_close(VSurf *s);

void vs_close(void *vs) {
    VSurf *s = vs;
    if (!s) return;
    if (s->fb_on) fb_close(s);
    free(s->clip_got);
    if (s->dpy) {
        if (s->img) { XDestroyImage(s->img); s->img = NULL; } /* frees its data too */
        if (s->ic) XDestroyIC(s->ic);
        if (s->im) XCloseIM(s->im);
        for (int i = 0; i < 11; i++)
            if (s->cursors[i]) XFreeCursor(s->dpy, s->cursors[i]);
        XFreePixmap(s->dpy, s->back);
        XFreeGC(s->dpy, s->gc);
        XFreeGC(s->dpy, s->pgc);
        XDestroyWindow(s->dpy, s->win);
        XCloseDisplay(s->dpy);
    }
    free(s);
}

void vs_clip_set(void *vs, int x, int y, int w, int h) {
    VSurf *s = vs;
    if (s->fb_on) {
        s->fb_clip_on = 1;
        s->fb_cx0 = x;
        s->fb_cy0 = y;
        s->fb_cx1 = x + (w > 0 ? w : 0);
        s->fb_cy1 = y + (h > 0 ? h : 0);
        return;
    }
    if (!s->dpy) return;
    XRectangle r;
    r.x = (short)x;
    r.y = (short)y;
    r.width = (unsigned short)(w > 0 ? w : 0);
    r.height = (unsigned short)(h > 0 ? h : 0);
    XSetClipRectangles(s->dpy, s->gc, 0, 0, &r, 1, Unsorted);
}

void vs_clip_clear(void *vs) {
    VSurf *s = vs;
    if (s->fb_on) {
        s->fb_clip_on = 0;
        return;
    }
    if (!s->dpy) return;
    XSetClipMask(s->dpy, s->gc, None);
}

int vs_width(void *vs) { return ((VSurf *)vs)->w; }
int vs_height(void *vs) { return ((VSurf *)vs)->h; }

void vs_set_title(void *vs, const char *t) {
    VSurf *s = vs;
    if (s->dpy) XStoreName(s->dpy, s->win, t);
}

/* Font cursors exist on every X server; HIDDEN is a 1x1 cursor whose mask is
   empty. Created per shape on first use, per surface (each surface has its
   own Display connection). */
void vs_set_cursor(void *vs, int shape) {
    VSurf *s = vs;
    if (!s) return;
    if (shape < 0 || shape > VS_CURSOR_HIDDEN) shape = VS_CURSOR_ARROW;
    s->in.cursor = shape;
    if (!s->dpy) return;
    if (!s->cursors[shape]) {
        static const unsigned int glyph[10] = {
            XC_left_ptr, XC_xterm, XC_hand2, XC_watch, XC_crosshair,
            XC_sb_h_double_arrow, XC_sb_v_double_arrow,
            XC_bottom_right_corner, XC_bottom_left_corner, XC_fleur
        };
        if (shape == VS_CURSOR_HIDDEN) {
            static char empty[1];
            Pixmap pm = XCreateBitmapFromData(s->dpy, s->win, empty, 1, 1);
            XColor black;
            memset(&black, 0, sizeof black);
            s->cursors[shape] = XCreatePixmapCursor(s->dpy, pm, pm, &black, &black, 0, 0);
            XFreePixmap(s->dpy, pm);
        } else {
            s->cursors[shape] = XCreateFontCursor(s->dpy, glyph[shape]);
        }
    }
    XDefineCursor(s->dpy, s->win, s->cursors[shape]);
    XFlush(s->dpy);
}
int vs_get_cursor(void *vs) { VSurf *s = vs; return s ? s->in.cursor : 0; }

/* X11 input methods position their own windows from the XIC's spot, which
   this backend does not set up (XIMPreeditNothing). */
void vs_set_ime_pos(void *vs, int x, int y, int h) { (void)vs; (void)x; (void)y; (void)h; }

/* _GTK_THEME_VARIANT is what GNOME-family window managers read to draw a dark
   frame; others ignore it, which is the right failure. */
void vs_set_dark_frame(void *vs, int on) {
    VSurf *s = vs;
    if (!s || !s->dpy) return;
    Atom a = XInternAtom(s->dpy, "_GTK_THEME_VARIANT", False);
    if (on) {
        Atom utf8 = XInternAtom(s->dpy, "UTF8_STRING", False);
        XChangeProperty(s->dpy, s->win, a, utf8, 8, PropModeReplace,
                        (const unsigned char *)"dark", 4);
    } else {
        XDeleteProperty(s->dpy, s->win, a);
    }
    XFlush(s->dpy);
}

/* _NET_WM_ICON is width, height, then one ARGB pixel per CARDINAL, top row
   first. Xlib passes format-32 data as longs, whatever their width, so the
   array is unsigned long rather than uint32. Headless and fbdev have no
   window manager to show an icon. */
void vs_set_icon(void *vs, const unsigned int *argb, int w, int h) {
    VSurf *s = vs;
    if (!s || !s->dpy || !argb || w <= 0 || h <= 0 || w > 256 || h > 256) return;
    size_t n = 2 + (size_t)w * (size_t)h;
    unsigned long *data = malloc(n * sizeof *data);
    if (!data) return;
    data[0] = (unsigned long)w;
    data[1] = (unsigned long)h;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) data[2 + i] = argb[i];
    Atom a = XInternAtom(s->dpy, "_NET_WM_ICON", False);
    XChangeProperty(s->dpy, s->win, a, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)data, (int)n);
    XFlush(s->dpy);
    free(data);
}


/* _MOTIF_WM_HINTS is the de-facto way to ask for an undecorated window: every
   major WM honours it, and unlike override-redirect the window stays managed,
   so it still gets focus, taskbar presence and _NET_WM_MOVERESIZE. */
void vs_set_decorated(void *vs, int on) {
    VSurf *s = vs;
    if (!s || !s->dpy) return;
    Atom a = XInternAtom(s->dpy, "_MOTIF_WM_HINTS", False);
    /* MotifWmHints: flags, functions, decorations, input_mode, status.
       flags = MWM_HINTS_DECORATIONS (1<<1). */
    long hints[5] = { 2, 0, 0, 0, 0 };
    hints[2] = on ? 1 : 0;
    XChangeProperty(s->dpy, s->win, a, a, 32, PropModeReplace,
                    (unsigned char *)hints, 5);
    XFlush(s->dpy);
}

void vs_get_position(void *vs, int *x, int *y) {
    VSurf *s = vs;
    if (x) *x = 0;
    if (y) *y = 0;
    if (!s || !s->dpy) return;
    /* The window's own x/y are relative to its parent, which for a managed
       window is the WM's frame — so translate to the root instead. */
    Window child;
    int rx = 0, ry = 0;
    if (XTranslateCoordinates(s->dpy, s->win, DefaultRootWindow(s->dpy),
                              0, 0, &rx, &ry, &child)) {
        if (x) *x = rx;
        if (y) *y = ry;
    }
}

void vs_move(void *vs, int x, int y) {
    VSurf *s = vs;
    if (!s || !s->dpy) return;
    XMoveWindow(s->dpy, s->win, x, y);
    XFlush(s->dpy);
}

void vs_set_size(void *vs, int w, int h) {
    VSurf *s = vs;
    if (!s || !s->dpy || w < 1 || h < 1) return;
    XResizeWindow(s->dpy, s->win, (unsigned)w, (unsigned)h);
    XFlush(s->dpy);
}

int vs_set_fullscreen(void *vs, int on) {
    VSurf *s = vs;
    if (!s || !s->dpy) return 0;
    Atom st = XInternAtom(s->dpy, "_NET_WM_STATE", True);
    Atom fs = XInternAtom(s->dpy, "_NET_WM_STATE_FULLSCREEN", True);
    if (st == None || fs == None) return 0;   /* WM does not advertise it */

    /* Ask through a client message rather than setting the property directly:
       the property is the WM's to own once the window is mapped, and only the
       message is honoured after that. */
    XClientMessageEvent e;
    memset(&e, 0, sizeof e);
    e.type = ClientMessage;
    e.window = s->win;
    e.message_type = st;
    e.format = 32;
    e.data.l[0] = on ? 1 : 0;   /* _NET_WM_STATE_ADD / _REMOVE */
    e.data.l[1] = (long)fs;
    e.data.l[2] = 0;
    e.data.l[3] = 1;            /* source: normal application */
    XSendEvent(s->dpy, DefaultRootWindow(s->dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&e);
    XFlush(s->dpy);
    return 1;
}

/* XRandR's monitor record. Declared here rather than including Xrandr.h so the
   header is not a build dependency either — only the first fields are read, and
   they have been stable since the extension gained XRRGetMonitors in 1.5. */
typedef struct {
    unsigned long name;
    int primary, automatic, noutput;
    int x, y, width, height;
    int mwidth, mheight;
    void *outputs;
} VsRRMonitor;

/* Xlib's default error handler PRINTS and EXITS, so a stale or foreign XID —
   exactly what a capture API gets handed — would take the process down. Swallow
   errors for the duration of the call instead and report failure the documented
   way, with a 0 return. */
static int vs_cap_failed;
static int vs_cap_err(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    vs_cap_failed = 1;
    return 0;
}

int vs_capture(unsigned long win, int *out, int cap, int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!win) return 0;
    /* Own connection: the caller may be naming a window from another process,
       and this is a query rather than anything tied to one of our surfaces. */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 0;
    vs_cap_failed = 0;
    XErrorHandler prev = XSetErrorHandler(vs_cap_err);

    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, (Window)win, &a) || vs_cap_failed ||
        a.map_state != IsViewable || a.width < 1 || a.height < 1) {
        /* An unmapped or iconified window has no content to read. */
        XSetErrorHandler(prev);
        XCloseDisplay(dpy);
        return 0;
    }
    if (w) *w = a.width;
    if (h) *h = a.height;
    int total = a.width * a.height;
    if (!out || cap <= 0) {
        XSetErrorHandler(prev);
        XCloseDisplay(dpy);
        return total;
    }

    XImage *im = XGetImage(dpy, (Window)win, 0, 0,
                           (unsigned)a.width, (unsigned)a.height,
                           AllPlanes, ZPixmap);
    if (!im || vs_cap_failed) {
        if (im) XDestroyImage(im);
        XSetErrorHandler(prev);
        XCloseDisplay(dpy);
        return 0;
    }

    int n = total < cap ? total : cap;
    /* Fast path for the ordinary 24/32-bit little-endian visual; XGetPixel
       otherwise, which is correct everywhere but goes through Xlib per pixel. */
    if (im->bits_per_pixel == 32 && im->byte_order == LSBFirst &&
        im->red_mask == 0xFF0000 && im->green_mask == 0x00FF00 &&
        im->blue_mask == 0x0000FF) {
        for (int y = 0; y < a.height; y++) {
            const uint32_t *row = (const uint32_t *)(im->data + (long)y * im->bytes_per_line);
            for (int x = 0; x < a.width; x++) {
                int i = y * a.width + x;
                if (i >= n) break;
                out[i] = (int)(row[x] & 0xFFFFFF);
            }
        }
    } else {
        for (int y = 0; y < a.height; y++)
            for (int x = 0; x < a.width; x++) {
                int i = y * a.width + x;
                if (i >= n) break;
                out[i] = (int)(XGetPixel(im, x, y) & 0xFFFFFF);
            }
    }
    XDestroyImage(im);
    XSetErrorHandler(prev);
    XCloseDisplay(dpy);
    return vs_cap_failed ? 0 : total;
}

int vs_monitors(VsMonitor *out, int max) {
    if (!out) max = 0;   /* counting call: fill nothing, just report how many */
    if (headless_on()) return headless_monitors(out, max);
    /* Open our own connection: this is a process-level query, and a caller may
       have no surface yet — placing a window before creating it is the point. */
    Display *dpy = XOpenDisplay(NULL);
    /* No display server at all — fbdev, or a bare CI box. One synthetic screen
       beats none: the contract is that this never returns zero monitors. */
    if (!dpy) return headless_monitors(out, max);
    int scr = DefaultScreen(dpy);
    int count = 0;

    /* dlopen rather than link: see the header. A machine without XRandR still
       works, reporting the single screen X can describe without it.
       The handle is deliberately never dlclosed: Xlib registers per-extension
       close hooks that live in libXrandr, and XCloseDisplay calls them — unload
       the library first and that call lands in freed text. Leaking one handle
       for the process lifetime is the correct trade. */
    static void *lib;
    static int tried;
    if (!tried) { tried = 1; lib = dlopen("libXrandr.so.2", RTLD_LAZY); }
    if (lib) {
        VsRRMonitor *(*get_monitors)(Display *, Window, int, int *) =
            (VsRRMonitor *(*)(Display *, Window, int, int *))
            dlsym(lib, "XRRGetMonitors");
        void (*free_monitors)(VsRRMonitor *) =
            (void (*)(VsRRMonitor *))dlsym(lib, "XRRFreeMonitors");
        if (get_monitors) {
            int n = 0;
            VsRRMonitor *m = get_monitors(dpy, DefaultRootWindow(dpy), 1, &n);
            if (m && n > 0) {
                count = n;
                int xft = x11_xft_scale(dpy);   /* one global resource */
                for (int i = 0; out && i < n && i < max; i++) {
                    out[i].x = m[i].x;
                    out[i].y = m[i].y;
                    out[i].w = m[i].width;
                    out[i].h = m[i].height;
                    out[i].primary = m[i].primary ? 1 : 0;
                    /* $VYTO_SCALE per monitor first, then the desktop's own
                       declaration, then this monitor's physical DPI. Xft.dpi
                       outranks physics deliberately: a 142dpi laptop panel is
                       routinely run at 96, and preferring the physical number
                       would oversize every window by ~48%. */
                    int sc = scale_from_env_at(i);
                    if (!sc) sc = xft;
                    if (!sc) sc = dpi_scale_pct(m[i].width, m[i].mwidth);
                    out[i].scale_pct = sc > 0 ? sc : 100;
                }
                if (free_monitors) free_monitors(m);
                XCloseDisplay(dpy);
                return count;
            }
            if (m && free_monitors) free_monitors(m);
        }
    }

    /* No XRandR: the display is one screen as far as we can tell. */
    count = 1;
    if (max > 0) {
        out[0].x = 0;
        out[0].y = 0;
        out[0].w = DisplayWidth(dpy, scr);
        out[0].h = DisplayHeight(dpy, scr);
        out[0].primary = 1;
        int sc = scale_from_env_at(0);
        if (!sc) sc = x11_detect_scale(dpy);
        out[0].scale_pct = sc > 0 ? sc : 100;
    }
    XCloseDisplay(dpy);
    return count;
}

int vs_drag_start(void *vs, int edge, int x_root, int y_root) {
    VSurf *s = vs;
    if (!s || !s->dpy) return 0;
    if (edge < 0 || edge > 8) return 0;
    Atom mr = XInternAtom(s->dpy, "_NET_WM_MOVERESIZE", True);
    if (mr == None) return 0;   /* WM does not advertise it */

    /* The button must be released first: the WM takes a fresh grab, and the
       one this client already holds from the press would fight it. */
    XUngrabPointer(s->dpy, CurrentTime);
    XFlush(s->dpy);

    XClientMessageEvent e;
    memset(&e, 0, sizeof e);
    e.type = ClientMessage;
    e.window = s->win;
    e.message_type = mr;
    e.format = 32;
    e.data.l[0] = x_root;
    e.data.l[1] = y_root;
    e.data.l[2] = edge;
    e.data.l[3] = 1;    /* button 1 */
    e.data.l[4] = 1;    /* source: normal application */
    XSendEvent(s->dpy, DefaultRootWindow(s->dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&e);
    XFlush(s->dpy);
    return 1;
}

void vs_set_min_size(void *vs, int w, int h) {
    VSurf *s = vs;
    if (!s || !s->dpy || w < 1 || h < 1) return;
    /* XAllocSizeHints rather than a stack struct: the hints carry fields we do
       not set, and the WM reads all of them. */
    XSizeHints *sh = XAllocSizeHints();
    if (!sh) return;
    sh->flags = PMinSize;
    sh->min_width = w;
    sh->min_height = h;
    XSetWMNormalHints(s->dpy, s->win, sh);
    XFree(sh);
}

void vs_fill_rect(void *vs, int x, int y, int w, int h, int rgb) {
    VSurf *s = vs;
    if (s->fb_on) {
        fb_fill(s, x, y, w, h, rgb);
        return;
    }
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XFillRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)w, (unsigned)h);
}

void vs_draw_rect(void *vs, int x, int y, int w, int h, int rgb) {
    VSurf *s = vs;
    if (s->fb_on) {
        fb_rect(s, x, y, w, h, rgb);
        return;
    }
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XDrawRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

void vs_draw_line(void *vs, int x0, int y0, int x1, int y1, int rgb) {
    VSurf *s = vs;
    if (s->fb_on) {
        fb_line(s, x0, y0, x1, y1, rgb);
        return;
    }
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XDrawLine(s->dpy, s->back, s->gc, x0, y0, x1, y1);
}

/* Decode a UTF-8 string into XChar2b (16-bit) glyph indices so text can be
   drawn with XDrawString16 against an iso10646-1 (Unicode) core font. XChar2b
   is BMP-only; code points outside the BMP (or malformed bytes) fold to U+FFFD.
   Returns the glyph count and heap-allocates *out (caller frees). Worst case is
   one glyph per byte, so strlen+1 slots always suffice. */
static int utf8_to_xchar2b(const char *str, XChar2b **out) {
    size_t n = strlen(str);
    XChar2b *buf = malloc((n + 1) * sizeof(XChar2b));
    if (!buf) { *out = NULL; return 0; }
    int count = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)str[i];
        unsigned int cp;
        int len;
        if (c < 0x80)              { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else                       { cp = 0xFFFD;   len = 1; }
        int ok = 1;
        for (int k = 1; k < len; k++) {
            if (i + (size_t)k >= n || ((unsigned char)str[i + k] & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | ((unsigned char)str[i + k] & 0x3F);
        }
        if (!ok) { cp = 0xFFFD; len = 1; }
        if (cp > 0xFFFF) cp = 0xFFFD;   /* XChar2b cannot address beyond the BMP */
        buf[count].byte1 = (unsigned char)(cp >> 8);
        buf[count].byte2 = (unsigned char)(cp & 0xFF);
        count++;
        i += (size_t)len;
    }
    *out = buf;
    return count;
}

void vs_draw_text(void *vs, int x, int y, const char *str, int rgb) {
    VSurf *s = vs;
    if (s->fb_on) {
        fb_text(s, x, y, str, rgb);
        return;
    }
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XChar2b *g;
    int n = utf8_to_xchar2b(str, &g);
    if (!g) { XDrawString(s->dpy, s->back, s->gc, x, y, str, (int)strlen(str)); return; }
    XDrawString16(s->dpy, s->back, s->gc, x, y, g, n);
    free(g);
}

/* per-surface staging XImage, (re)sized on demand */
static XImage *blit_image(VSurf *s, int w, int h) {
    if (s->img && s->img_w >= w && s->img_h >= h) return s->img;
    int nw = s->img_w > w ? s->img_w : w;
    int nh = s->img_h > h ? s->img_h : h;
    if (s->img) { XDestroyImage(s->img); s->img = NULL; } /* frees the data too */
    char *buf = malloc((size_t)nw * (size_t)nh * 4);
    if (!buf) return NULL;
    int scr = DefaultScreen(s->dpy);
    s->img = XCreateImage(s->dpy, DefaultVisual(s->dpy, scr),
                          (unsigned)DefaultDepth(s->dpy, scr), ZPixmap, 0, buf,
                          (unsigned)nw, (unsigned)nh, 32, 0);
    if (!s->img) { free(buf); return NULL; }
    s->img_w = nw;
    s->img_h = nh;
    return s->img;
}

/* nearest-neighbor blit of a 0x00RRGGBB pixel buffer into the backbuffer. */
void vs_blit(void *vs, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth) {
    VSurf *s = vs;
    if (srcw <= 0 || srch <= 0 || dstw <= 0 || dsth <= 0) return;
    if (s->fb_on) {
        fb_blit(s, pixels, srcw, srch, dstx, dsty, dstw, dsth);
        return;
    }
    if (!s->dpy) return;
    XImage *img = blit_image(s, dstw, dsth);
    if (!img) return;
    /* Fast path: on a little-endian 32bpp TrueColor visual the image row is a
       plain uint32 array of 0x00RRGGBB — pack directly, skipping 786K XPutPixel
       calls per frame. Falls back to XPutPixel on paletted/odd visuals. */
    if (truecolor && img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        for (int dy = 0; dy < dsth; dy++) {
            int sy = dy * srch / dsth;
            const int *src = pixels + (long)sy * srcw;
            uint32_t *row = (uint32_t *)(img->data + (long)dy * img->bytes_per_line);
            if (srcw == dstw) {
                for (int dx = 0; dx < dstw; dx++) row[dx] = (uint32_t)(src[dx] & 0xFFFFFF);
            } else {
                for (int dx = 0; dx < dstw; dx++)
                    row[dx] = (uint32_t)(src[dx * srcw / dstw] & 0xFFFFFF);
            }
        }
    } else {
        for (int dy = 0; dy < dsth; dy++) {
            int sy = dy * srch / dsth;
            for (int dx = 0; dx < dstw; dx++)
                XPutPixel(img, dx, dy, pixel_of(s->dpy, pixels[(long)sy * srcw + dx * srcw / dstw]));
        }
    }
    XPutImage(s->dpy, s->back, s->pgc, img, 0, 0, dstx, dsty, (unsigned)dstw, (unsigned)dsth);
}

/* unscaled sub-rect copy with explicit source stride (pixels), for
   dirty-region presents out of a full-frame canvas */
void vs_blit_rect(void *vs, const int *pixels, int stride_px,
                  int srcx, int srcy, int w, int h, int dstx, int dsty) {
    VSurf *s = vs;
    if (w <= 0 || h <= 0 || stride_px <= 0) return;
    if (s->fb_on) {
        fb_blit_sub(s, pixels, stride_px, srcx, srcy, w, h, dstx, dsty);
        return;
    }
    if (!s->dpy) return;
    XImage *img = blit_image(s, w, h);
    if (!img) return;
    if (truecolor && img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        for (int dy = 0; dy < h; dy++) {
            const int *src = pixels + (long)(srcy + dy) * stride_px + srcx;
            uint32_t *row = (uint32_t *)(img->data + (long)dy * img->bytes_per_line);
            for (int dx = 0; dx < w; dx++) row[dx] = (uint32_t)(src[dx] & 0xFFFFFF);
        }
    } else {
        for (int dy = 0; dy < h; dy++)
            for (int dx = 0; dx < w; dx++)
                XPutPixel(img, dx, dy,
                          pixel_of(s->dpy, pixels[(long)(srcy + dy) * stride_px + srcx + dx]));
    }
    XPutImage(s->dpy, s->back, s->pgc, img, 0, 0, dstx, dsty, (unsigned)w, (unsigned)h);
}

void vs_present(void *vs) {
    VSurf *s = vs;
    if (s->fb_on) {
        fb_flush(s, 0, 0, s->w, s->h);
        return;
    }
    if (!s->dpy) return;
    XCopyArea(s->dpy, s->back, s->win, s->pgc, 0, 0, (unsigned)s->w, (unsigned)s->h, 0, 0);
    XFlush(s->dpy);
}

void vs_present_rect(void *vs, int x, int y, int w, int h) {
    VSurf *s = vs;
    if (s->fb_on) {
        fb_flush(s, x, y, w, h);
        return;
    }
    if (!s->dpy) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    XCopyArea(s->dpy, s->back, s->win, s->pgc, x, y, (unsigned)w, (unsigned)h, x, y);
    XFlush(s->dpy);
}

int vs_text_width(void *vs, const char *str) {
    VSurf *s = vs;
    if (s->fb_on) return FB_FONT_W * (int)strlen(str);
    if (!s->dpy || !font) return 9 * (int)strlen(str);
    XChar2b *g;
    int n = utf8_to_xchar2b(str, &g);
    if (!g) return XTextWidth(font, str, (int)strlen(str));
    int w = XTextWidth16(font, g, n);
    free(g);
    return w;
}

int vs_font_ascent(void *vs) {
    VSurf *s = vs;
    if (s->fb_on) return FB_FONT_ASCENT;
    if (!s->dpy || !font) return 11;
    return font->ascent;
}

int vs_font_height(void *vs) {
    VSurf *s = vs;
    if (s->fb_on) return FB_FONT_H;
    if (!s->dpy || !font) return 15;
    return font->ascent + font->descent;
}

/* X modifier state -> VS_MOD_* bitmask (Mod1 = Alt, Mod4 = Super on
   stock X keyboard maps) */
static int x11_mods_of(unsigned int state) {
    int m = 0;
    if (state & ShiftMask) m |= VS_MOD_SHIFT;
    if (state & ControlMask) m |= VS_MOD_CTRL;
    if (state & Mod1Mask) m |= VS_MOD_ALT;
    if (state & Mod4Mask) m |= VS_MOD_SUPER;
    return m;
}

/* answer another client's paste request out of clip_local (we own CLIPBOARD) */
static void clip_serve_request(VSurf *s, XSelectionRequestEvent *rq) {
    XSelectionEvent n;
    memset(&n, 0, sizeof n);
    n.type = SelectionNotify;
    n.display = rq->display;
    n.requestor = rq->requestor;
    n.selection = rq->selection;
    n.target = rq->target;
    n.time = rq->time;
    n.property = None; /* refused unless a branch below accepts */
    if (clip_local) {
        Atom prop = rq->property != None ? rq->property : rq->target;
        if (rq->target == s->a_targets) {
            Atom types[2];
            types[0] = s->a_utf8;
            types[1] = XA_STRING;
            XChangeProperty(rq->display, rq->requestor, prop, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)types, 2);
            n.property = prop;
        } else if (rq->target == s->a_utf8 || rq->target == XA_STRING) {
            XChangeProperty(rq->display, rq->requestor, prop, rq->target, 8,
                            PropModeReplace, (unsigned char *)clip_local,
                            (int)strlen(clip_local));
            n.property = prop;
        }
    }
    XSendEvent(rq->display, rq->requestor, False, 0, (XEvent *)&n);
    XFlush(rq->display);
}

/* read the converted selection out of our VS_CLIP property into clip_got */
static const char *clip_read_property(VSurf *s) {
    Atom type = None;
    int fmt = 0;
    unsigned long len = 0, left = 0;
    unsigned char *data = NULL;
    /* 1 MiB cap — clipboard text, not a file transfer */
    if (XGetWindowProperty(s->dpy, s->win, s->a_clip_prop, 0, 1 << 18, True,
                           AnyPropertyType, &type, &fmt, &len, &left,
                           &data) != Success)
        return "";
    if (!data) return "";
    if (type == s->a_incr || fmt != 8) {
        /* INCR (incremental transfer) unsupported: too big, treat as empty */
        XFree(data);
        return "";
    }
    char *p = realloc(s->clip_got, len + 1);
    if (!p) { XFree(data); return ""; }
    memcpy(p, data, len);
    p[len] = 0;
    s->clip_got = p;
    XFree(data);
    return s->clip_got;
}

void vs_clipboard_set(void *vs, const char *text) {
    VSurf *s = vs;
    if (!text) text = "";
    clip_store(&clip_local, text); /* also the "we own it" backing store */
    if (!s->dpy) return;
    XSetSelectionOwner(s->dpy, s->a_clipboard, s->win, CurrentTime);
    XFlush(s->dpy);
}

const char *vs_clipboard_get(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return clip_local ? clip_local : "";
    if (XGetSelectionOwner(s->dpy, s->a_clipboard) == s->win)
        return clip_local ? clip_local : "";
    /* ask the owner for UTF-8, falling back to STRING; pump selection
       traffic only (other events stay queued for vs_wait), give up after
       ~300ms so a dead owner can't hang the app */
    Atom target = s->a_utf8;
    XDeleteProperty(s->dpy, s->win, s->a_clip_prop);
    XConvertSelection(s->dpy, s->a_clipboard, target, s->a_clip_prop, s->win,
                      CurrentTime);
    XFlush(s->dpy);
    int fd = ConnectionNumber(s->dpy);
    for (int waited = 0; waited <= 300;) {
        XEvent e;
        while (XCheckTypedWindowEvent(s->dpy, s->win, SelectionNotify, &e)) {
            if (e.xselection.selection != s->a_clipboard) continue;
            if (e.xselection.property != None) return clip_read_property(s);
            if (target == s->a_utf8) { /* owner refused UTF-8: retry STRING */
                target = XA_STRING;
                XConvertSelection(s->dpy, s->a_clipboard, target, s->a_clip_prop,
                                  s->win, CurrentTime);
                XFlush(s->dpy);
                continue;
            }
            return "";
        }
        while (XCheckTypedWindowEvent(s->dpy, s->win, SelectionRequest, &e))
            clip_serve_request(s, &e.xselectionrequest);
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20 * 1000;
        if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) waited += 20;
    }
    return "";
}

/* decode an X key event into s->in.key/s->in.text; 1 if a key we deliver, else 0 */
static int x11_decode_key(VSurf *s, XKeyEvent *ke) {
    char buf[16];
    KeySym ks = 0;
    int n;
    if (s->ic && ke->type == KeyPress) {
        /* UTF-8 text input via the input method (composed chars, dead keys) */
        Status st = 0;
        n = Xutf8LookupString(s->ic, ke, buf, sizeof buf - 1, &ks, &st);
        if (st != XLookupChars && st != XLookupBoth && st != XLookupKeySym) n = 0;
        if (st == XLookupKeySym) n = 0;
    } else {
        n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL);
    }
    buf[n < 0 ? 0 : n] = 0;
    s->in.text[0] = 0;
    switch (ks) {
    case XK_Return: case XK_KP_Enter: s->in.key = VS_KEY_ENTER; return 1;
    case XK_BackSpace: s->in.key = VS_KEY_BACKSPACE; return 1;
    case XK_Escape: s->in.key = VS_KEY_ESC; return 1;
    case XK_Up: case XK_KP_Up: s->in.key = VS_KEY_UP; return 1;
    case XK_Down: case XK_KP_Down: s->in.key = VS_KEY_DOWN; return 1;
    case XK_Left: case XK_KP_Left: s->in.key = VS_KEY_LEFT; return 1;
    case XK_Right: case XK_KP_Right: s->in.key = VS_KEY_RIGHT; return 1;
    case XK_Delete: case XK_KP_Delete: s->in.key = VS_KEY_DELETE; return 1;
    case XK_Tab: case XK_ISO_Left_Tab: s->in.key = VS_KEY_TAB; return 1;
    case XK_Home: case XK_KP_Home: s->in.key = VS_KEY_HOME; return 1;
    case XK_End: case XK_KP_End: s->in.key = VS_KEY_END; return 1;
    case XK_Page_Up: case XK_KP_Page_Up: s->in.key = VS_KEY_PAGEUP; return 1;
    case XK_Page_Down: case XK_KP_Page_Down: s->in.key = VS_KEY_PAGEDOWN; return 1;
    case XK_Insert: case XK_KP_Insert: s->in.key = VS_KEY_INSERT; return 1;
    case XK_Shift_L: case XK_Shift_R: s->in.key = VS_KEY_SHIFT; return 1;
    case XK_Control_L: case XK_Control_R: s->in.key = VS_KEY_CTRL; return 1;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R:
        s->in.key = VS_KEY_ALT; return 1;
    case XK_Super_L: case XK_Super_R: s->in.key = VS_KEY_SUPER; return 1;
    default:
        if (ks >= XK_F1 && ks <= XK_F12) { /* contiguous keysym block */
            s->in.key = VS_KEY_F1 + (int)(ks - XK_F1);
            return 1;
        }
        /* Printable key: prefer the (shift-aware) keysym so Ctrl+A still
           reads as 'a' — the lookup folds Ctrl into a control byte, which
           would otherwise swallow the shortcut key entirely. Text is set
           only from a printable lookup, so Ctrl-chorded keys deliver a key
           code but no insertable text. */
        if (ks == XK_space || (ks >= 0x21 && ks <= 0x7e)) {
            s->in.key = (int)ks;
            if (n >= 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
                if ((size_t)n < sizeof s->in.text) memcpy(s->in.text, buf, (size_t)n + 1);
            }
            return 1;
        }
        /* keypad digits, and any UTF-8 text the input method composed:
           no ASCII key code (key = 0 for multibyte), only insertable text */
        if (n >= 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
            s->in.key = (n == 1 && (unsigned char)buf[0] < 127) ? buf[0] : 0;
            if ((size_t)n < sizeof s->in.text) memcpy(s->in.text, buf, (size_t)n + 1);
            return 1;
        }
    }
    return 0; /* dead key or unmapped */
}

/* Drain a run of pending ConfigureNotify, keeping only the last size. Called
   with the size of the configure being translated; leaves the newest in *w/*h.
   Peeks before consuming, so an event of any other type stops the drain and
   stays queued. */
static void x11_coalesce_configure(VSurf *s, int *w, int *h) {
    while (XPending(s->dpy)) {
        XEvent e;
        XPeekEvent(s->dpy, &e);
        if (e.type != ConfigureNotify) return;
        XNextEvent(s->dpy, &e);
        *w = e.xconfigure.width;
        *h = e.xconfigure.height;
    }
}

/* translate one XEvent into a VS_EV_* code, or -1 to ignore and keep waiting */
/* Middle and the two thumb buttons. 8 and 9 are back and forward on every
   mouse X has a driver for; Xlib has no names for them. Right is not here:
   its press is MOUSE_RDOWN, only its release is a BUTTON_UP. */
static int x11_button_of(unsigned int b) {
    if (b == Button2) return VS_BUTTON_MIDDLE;
    if (b == 8) return VS_BUTTON_BACK;
    if (b == 9) return VS_BUTTON_FORWARD;
    return 0;
}

static int x11_translate(VSurf *s, XEvent *e) {
    switch (e->type) {
    case Expose: {
        /* The server splits damage into several Expose events and sets count to
           how many remain, so union them and deliver on the last. Repainting
           only that union is much cheaper than a full redraw when a small strip
           was uncovered. */
        int ex = e->xexpose.x, ey = e->xexpose.y;
        int ew = e->xexpose.width, eh = e->xexpose.height;
        if (s->dmg_w <= 0 || s->dmg_h <= 0) {
            s->dmg_x = ex; s->dmg_y = ey; s->dmg_w = ew; s->dmg_h = eh;
        } else {
            int x0 = s->dmg_x < ex ? s->dmg_x : ex;
            int y0 = s->dmg_y < ey ? s->dmg_y : ey;
            int x1 = s->dmg_x + s->dmg_w > ex + ew ? s->dmg_x + s->dmg_w : ex + ew;
            int y1 = s->dmg_y + s->dmg_h > ey + eh ? s->dmg_y + s->dmg_h : ey + eh;
            s->dmg_x = x0; s->dmg_y = y0; s->dmg_w = x1 - x0; s->dmg_h = y1 - y0;
        }
        if (e->xexpose.count == 0) return VS_EV_EXPOSE;
        return -1;
    }
    case ConfigureNotify: {
        int w = e->xconfigure.width, h = e->xconfigure.height;
        /* A drag-resize delivers configures at pointer rate, and each one that
           reaches the caller costs a full canvas rebuild, relayout and repaint.
           Only the final size matters, so swallow the run first — the same
           trick x11_coalesce_motion plays on MotionNotify. */
        x11_coalesce_configure(s, &w, &h);
        if (w != s->w || h != s->h) {
            s->w = w;
            s->h = h;
            XFreePixmap(s->dpy, s->back);
            s->back = XCreatePixmap(s->dpy, s->win, (unsigned)w, (unsigned)h,
                                    (unsigned)DefaultDepth(s->dpy, DefaultScreen(s->dpy)));
            /* XCreatePixmap does not clear: the new pixmap holds whatever that
               server memory last contained. A present racing the repaint would
               show that garbage, so start it from a known colour. */
            XSetForeground(s->dpy, s->pgc, BlackPixel(s->dpy, DefaultScreen(s->dpy)));
            XFillRectangle(s->dpy, s->back, s->pgc, 0, 0, (unsigned)w, (unsigned)h);
            /* the whole buffer is undefined now, so any damage accumulated
               against the old geometry is meaningless — a resize repaints in
               full regardless */
            s->dmg_w = 0;
            s->dmg_h = 0;
            return VS_EV_RESIZE;
        }
        return -1;
    }
    case KeyPress:
        s->in.mods = x11_mods_of(e->xkey.state);
        return x11_decode_key(s, &e->xkey) ? VS_EV_KEY : -1;
    case KeyRelease:
        s->in.mods = x11_mods_of(e->xkey.state);
        return x11_decode_key(s, &e->xkey) ? VS_EV_KEY_UP : -1;
    case ButtonPress:
        s->in.mods = x11_mods_of(e->xbutton.state);
        if (e->xbutton.button == Button1) {
            s->in.x = e->xbutton.x;
            s->in.y = e->xbutton.y;
            return VS_EV_MOUSE_DOWN;
        }
        if (e->xbutton.button == Button3) {
            s->in.x = e->xbutton.x;
            s->in.y = e->xbutton.y;
            return VS_EV_MOUSE_RDOWN;
        }
        if (e->xbutton.button == Button4 || e->xbutton.button == Button5) {
            s->in.x = e->xbutton.x;
            s->in.y = e->xbutton.y;
            /* up = negative (content moves down), down = positive */
            s->in.wheel = e->xbutton.button == Button4 ? -3 : 3;
            s->in.wheel_x = 0;
            return VS_EV_MOUSE_WHEEL;
        }
        if (e->xbutton.button == 6 || e->xbutton.button == 7) {
            /* the horizontal wheel/tilt: 6 left, 7 right (no Xlib names) */
            s->in.x = e->xbutton.x;
            s->in.y = e->xbutton.y;
            s->in.wheel = 0;
            s->in.wheel_x = e->xbutton.button == 6 ? -3 : 3;
            return VS_EV_MOUSE_WHEEL;
        }
        {
            int b = x11_button_of(e->xbutton.button);
            if (b) {
                s->in.x = e->xbutton.x;
                s->in.y = e->xbutton.y;
                s->in.button = b;
                return VS_EV_BUTTON_DOWN;
            }
        }
        return -1;
    case ButtonRelease:
        s->in.mods = x11_mods_of(e->xbutton.state);
        if (e->xbutton.button == Button1) {
            s->in.x = e->xbutton.x;
            s->in.y = e->xbutton.y;
            return VS_EV_MOUSE_UP;
        }
        {
            int b = e->xbutton.button == Button3 ? VS_BUTTON_RIGHT
                                                 : x11_button_of(e->xbutton.button);
            if (b) {
                s->in.x = e->xbutton.x;
                s->in.y = e->xbutton.y;
                s->in.button = b;
                return VS_EV_BUTTON_UP;
            }
        }
        return -1;
    case MotionNotify:
        s->in.mods = x11_mods_of(e->xmotion.state);
        /* hover source — coalesce: only deliver if the cursor actually moved.
           X11 often fires many MotionNotify per pixel; throttle by storing the
           latest and letting vs_wait dedupe via the change check in Window. */
        s->in.x = e->xmotion.x;
        s->in.y = e->xmotion.y;
        return VS_EV_MOUSE_MOVE;
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == s->wm_delete) return VS_EV_CLOSE;
        return -1;
    case SelectionRequest: /* another client pasting what we copied */
        clip_serve_request(s, &e->xselectionrequest);
        return -1;
    case SelectionClear: /* someone else copied; nothing to do */
        return -1;
    default:
        return -1;
    }
}

/* Coalesce a burst of MotionNotify: after delivering one move, swallow any
   further queued motion events (keeping the newest coordinates) so a fast
   mouse produces one hover pass per frame instead of one per pixel. */
static void x11_coalesce_motion(VSurf *s) {
    while (XPending(s->dpy)) {
        XEvent e;
        XPeekEvent(s->dpy, &e);
        if (e.type != MotionNotify) return;
        XNextEvent(s->dpy, &e);
        s->in.x = e.xmotion.x;
        s->in.y = e.xmotion.y;
        s->in.mods = x11_mods_of(e.xmotion.state);
    }
}

/* The six input accessors, X11/fbdev/headless arm. NULL-guarded because
   raw_handle() is public, so a caller can hand us anything. */
int vs_key(void *vs)   { VSurf *s = vs; return s ? s->in.key : 0; }
const char *vs_text(void *vs) { VSurf *s = vs; return s ? s->in.text : ""; }
int vs_x(void *vs)     { VSurf *s = vs; return s ? s->in.x : 0; }
int vs_y(void *vs)     { VSurf *s = vs; return s ? s->in.y : 0; }
int vs_wheel(void *vs) { VSurf *s = vs; return s ? s->in.wheel : 0; }
int vs_mods(void *vs)  { VSurf *s = vs; return s ? s->in.mods : 0; }
int vs_wheel_x(void *vs) { VSurf *s = vs; return s ? s->in.wheel_x : 0; }
int vs_button(void *vs)  { VSurf *s = vs; return s ? s->in.button : 0; }

/* translate + IME filter + motion coalescing for one raw event */
static int x11_deliver(VSurf *s, XEvent *e) {
    if (XFilterEvent(e, None)) return -1; /* consumed by the input method */
    int r = x11_translate(s, e);
    if (r == VS_EV_MOUSE_MOVE) x11_coalesce_motion(s);
    return r;
}

int vs_wait(void *vs) {
    VSurf *s = vs;
    if (s->fb_on) return fb_next(s, 1, -1);
    if (!s->dpy) return headless_wait(&s->in, &s->w, &s->h);
    for (;;) {
        XEvent e;
        XNextEvent(s->dpy, &e);
        int r = x11_deliver(s, &e);
        if (r >= 0) return r;
    }
}

/* non-blocking: next pending event, or VS_EV_NONE if the queue is empty.
   Flushes output first so this frame's drawing is on screen. */
int vs_poll(void *vs) {
    VSurf *s = vs;
    if (s->fb_on) return fb_next(s, 0, -1);
    if (!s->dpy) return headless_wait(&s->in, &s->w, &s->h);
    XFlush(s->dpy);
    while (XPending(s->dpy)) {
        XEvent e;
        XNextEvent(s->dpy, &e);
        int r = x11_deliver(s, &e);
        if (r >= 0) return r;
    }
    return VS_EV_NONE;
}

/* block up to ms milliseconds for an event; VS_EV_TIMER if it elapses with
   none. The tick source for game loops. */
int vs_wait_timeout(void *vs, int ms) {
    VSurf *s = vs;
    if (s->fb_on) return fb_next(s, 1, ms >= 0 ? ms : 0);
    if (!s->dpy) return headless_wait(&s->in, &s->w, &s->h);
    int fd = ConnectionNumber(s->dpy);
    for (;;) {
        XFlush(s->dpy);
        while (XPending(s->dpy)) {
            XEvent e;
            XNextEvent(s->dpy, &e);
            int r = x11_deliver(s, &e);
            if (r >= 0) return r;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        int rc = select(fd + 1, &rf, NULL, NULL, &tv);
        if (rc <= 0) return VS_EV_TIMER; /* timeout or interrupted: tick */
        /* data ready: loop and drain via XPending */
    }
}

/* Block up to ms for a window event OR readability on any of `n` caller fds.
   Returns a VS_EV_* as vs_wait_timeout does; VS_EV_TIMER covers both "the
   timeout elapsed" and "one of your fds is ready", so the caller checks its
   own descriptors on every return regardless.

   ── WHY THIS EXISTS ─────────────────────────────────────────────────────────
   A GUI app waiting on work that finishes elsewhere -- a forked worker, a
   socket -- otherwise has to poll on a timer, because Surface owns the
   blocking call and knows nothing about the caller's descriptors. That is a
   wakeup every 16ms for as long as the work is outstanding.

   Folding the fds into the select the X11 backend was already doing lets the
   app block properly: it sleeps until the window says something or the work
   lands, and burns nothing in between.

   fds==NULL/n<=0 is exactly vs_wait_timeout, and the two share this body. */
int vs_wait_timeout_fds(void *vs, int ms, const int *fds, int n) {
    VSurf *s = vs;
    if (s->fb_on) return fb_next(s, 1, ms >= 0 ? ms : 0);
    if (!s->dpy) return headless_wait(&s->in, &s->w, &s->h);
    int xfd = ConnectionNumber(s->dpy);
    for (;;) {
        XFlush(s->dpy);
        while (XPending(s->dpy)) {
            XEvent e;
            XNextEvent(s->dpy, &e);
            int r = x11_deliver(s, &e);
            if (r >= 0) return r;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(xfd, &rf);
        int maxfd = xfd;
        for (int i = 0; i < n; i++) {
            int f = fds[i];
            /* FD_SETSIZE is the hard ceiling of select(2); a descriptor past it
               corrupts the caller's stack rather than failing, so skip it. A UI
               app watching >1024 fds wants vyto/os/reactor, not this. */
            if (f < 0 || f >= FD_SETSIZE) continue;
            FD_SET(f, &rf);
            if (f > maxfd) maxfd = f;
        }
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        int rc = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (rc <= 0) return VS_EV_TIMER;   /* timeout or interrupted: tick */
        /* One of the caller's fds, not the display: report a tick so the
           caller looks at its own descriptors. Checking the X fd explicitly
           (rather than assuming) keeps a ready worker from being mistaken for
           window input and sent round the XPending drain, which would block. */
        if (!FD_ISSET(xfd, &rf)) return VS_EV_TIMER;
        /* display data ready: loop and drain via XPending */
    }
}

/* The descriptor this surface's events arrive on, or -1 where the backend has
   none to give.

   X11 is the one backend with a real answer: the display connection is a
   socket, so an external event loop can select() on it alongside its own fds
   and call vs_poll() when it fires. That is what lets vyto/os/reactor drive a
   window and a set of sockets from ONE wait instead of polling the window on a
   timer.

   -1 is not a failure, it is "this backend cannot be waited on from outside":
   Win32 blocks in MsgWaitForMultipleObjectsEx on a message queue and Android
   on a pthread condvar, and neither is a descriptor. Callers fall back to a
   bounded vs_wait_timeout there, which is exactly what Window.add_poll_source
   already does. Adding this is additive to the ABI — no existing entry point
   changes shape.

   IMPORTANT: readable does NOT mean an event is ready. Xlib buffers, so bytes
   on the socket may be a partial message, and conversely XPending() can have
   events queued with nothing on the socket. A caller must check the queue
   before blocking, not only after the fd fires. */
int vs_event_fd(void *vs) {
    VSurf *s = vs;
    if (!s) return -1;
    if (s->fb_on) return -1;   /* fbdev: reads /dev/input, not a single fd */
    if (!s->dpy) return -1;    /* headless */
    return ConnectionNumber(s->dpy);
}

/* Consumes the region, so a second call before the next Expose reports none —
   the caller either repaints it now or loses it, which is the same contract the
   input getters have. fbdev and headless report no region and repaint fully. */
int vs_damage(void *vs, VsRect *out) {
    VSurf *s = vs;
    if (!s || !out || s->dmg_w <= 0 || s->dmg_h <= 0) return 0;
    out->x = s->dmg_x;
    out->y = s->dmg_y;
    out->w = s->dmg_w;
    out->h = s->dmg_h;
    s->dmg_w = 0;
    s->dmg_h = 0;
    return 1;
}

/* Events already decoded and waiting, independent of the socket. Xlib's own
   queue can be non-empty with nothing readable on the connection — the bytes
   were consumed by an earlier read — so an external loop that blocked purely
   on the fd would sleep with events in hand. Check this first, every pass. */
int vs_events_pending(void *vs) {
    VSurf *s = vs;
    if (!s || s->fb_on || !s->dpy) return 0;
    XFlush(s->dpy);
    return XPending(s->dpy);
}

/* Whether vs_wait_timeout_fds really waits on the caller's fds, or degrades to
   a bounded poll. True only on X11 (and only for a real window: fbdev and
   headless have no display select to fold into). A caller uses this to decide
   whether it may block indefinitely or must keep a timer ceiling. */
int vs_can_wait_fds(void *vs) {
    VSurf *s = vs;
    return (s && !s->fb_on && s->dpy) ? 1 : 0;
}

int vs_scale_pct(void *vs) {
    VSurf *s = vs;
    if (s && s->dpy) {
        int x = 0, y = 0;
        vs_get_position(s, &x, &y);
        int idx = monitor_index_at(x, y);
        int env = scale_from_env_at(idx);
        if (env) return env;
        VsMonitor mons[16];
        int n = vs_monitors(mons, 16);
        if (idx < n && idx < 16 && mons[idx].scale_pct > 0) return mons[idx].scale_pct;
    }
    int env = scale_from_env();
    if (env) return env;
    if (s && s->in.dpi_pct > 0) return s->in.dpi_pct; /* headless `scale N` */
    return g_scale_pct ? g_scale_pct : 100;
}

/* X11 has no per-window DPI-change notice, so only a headless `scale N` ever
   sets this here; see the Win32 arm. */
int vs_take_scale_change(void *vs) {
    VSurf *s = vs;
    if (!s || !s->in.scale_changed) return 0;
    s->in.scale_changed = 0;
    return 1;
}

void *vs_native_display(void *vs) { return ((VSurf *)vs)->dpy; }
unsigned long vs_native_window(void *vs) { return (unsigned long)((VSurf *)vs)->win; }
void *vs_native_gc(void *vs) { return ((VSurf *)vs)->gc; }

#endif /* _WIN32 / X11 */

/* No desktop arm drives a vsync clock: X11 has no portable frame callback
 * without a compositor extension, and Win32's DwmFlush blocks rather than
 * calling back. Returning 0 is not a stub — it is the contract that tells
 * Window.run() to keep presenting eagerly, exactly as it did before vsync
 * gating existed. Shared by the Win32, X11, fbdev and headless arms. */
int vs_set_vsync(void *vs, int on) {
    (void)vs;
    (void)on;
    return 0;
}

#endif /* !__ANDROID__ — Android is vsurf_android.c */
