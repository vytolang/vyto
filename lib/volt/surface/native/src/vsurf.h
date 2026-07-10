/* vsurf — Layer-0 surface shim for volt/surface: a pixel canvas plus a
 * classified event queue. X11 backend today; the API is backend-neutral so
 * SDL (or others) can slot in behind the same calls.
 *
 * Headless test backend: with VS_HEADLESS=1 in the environment, vs_open
 * connects to nothing, drawing is a no-op, metrics are fixed (9x15-like),
 * and vs_wait replays events scripted in the file named by VS_EVENTS. */
#ifndef VSURF_H
#define VSURF_H

/* event classes returned by vs_wait */
enum {
    VS_EV_NONE = 0,
    VS_EV_EXPOSE = 1,
    VS_EV_KEY = 2,
    VS_EV_MOUSE_DOWN = 3,
    VS_EV_MOUSE_UP = 4,
    VS_EV_RESIZE = 5,
    VS_EV_CLOSE = 6,
    VS_EV_MOUSE_MOVE = 7, /* reserved; not delivered in v1 */
    VS_EV_TIMER = 8,      /* vs_wait_timeout elapsed with no input (game tick) */
    VS_EV_KEY_UP = 9,     /* a key was released (vs_key() gives the code) */
    VS_EV_MOUSE_WHEEL = 10, /* mouse wheel scrolled (vs_wheel() gives the delta) */
    VS_EV_MOUSE_RDOWN = 11  /* right button pressed (vs_mouse_x/y() give position) */
};

/* simplified key codes from vs_key: printable ASCII, or one of these.
   VS_KEY_SPACE is plain ASCII 32, named for symmetry. Modifier keys are
   also delivered as their own KEY/KEY_UP events (for held-key game input);
   use vs_mods() for the "is shift/ctrl down during this event" question. */
enum {
    VS_KEY_SPACE = 32,
    VS_KEY_ENTER = 1000,
    VS_KEY_BACKSPACE = 1001,
    VS_KEY_ESC = 1002,
    VS_KEY_UP = 1003,
    VS_KEY_DOWN = 1004,
    VS_KEY_LEFT = 1005,
    VS_KEY_RIGHT = 1006,
    VS_KEY_DELETE = 1007,
    VS_KEY_TAB = 1008,
    VS_KEY_HOME = 1009,
    VS_KEY_END = 1010,
    VS_KEY_PAGEUP = 1011,
    VS_KEY_PAGEDOWN = 1012,
    VS_KEY_INSERT = 1013,
    VS_KEY_SHIFT = 1014,
    VS_KEY_CTRL = 1015,
    VS_KEY_ALT = 1016,
    VS_KEY_SUPER = 1017,
    VS_KEY_F1 = 1021,
    VS_KEY_F2 = 1022,
    VS_KEY_F3 = 1023,
    VS_KEY_F4 = 1024,
    VS_KEY_F5 = 1025,
    VS_KEY_F6 = 1026,
    VS_KEY_F7 = 1027,
    VS_KEY_F8 = 1028,
    VS_KEY_F9 = 1029,
    VS_KEY_F10 = 1030,
    VS_KEY_F11 = 1031,
    VS_KEY_F12 = 1032
};

/* modifier bitmask from vs_mods(): the modifier state at the time of the
   most recently delivered event (keys, mouse buttons, motion, wheel) */
enum {
    VS_MOD_SHIFT = 1,
    VS_MOD_CTRL = 2,
    VS_MOD_ALT = 4,
    VS_MOD_SUPER = 8
};

void *vs_open(const char *title, int w, int h); /* NULL on failure */
void vs_close(void *s);
int vs_width(void *s);
int vs_height(void *s);
void vs_set_title(void *s, const char *t);

/* drawing goes to a backbuffer; colors are 0xRRGGBB */
void vs_fill_rect(void *s, int x, int y, int w, int h, int rgb);
void vs_draw_rect(void *s, int x, int y, int w, int h, int rgb);
void vs_draw_line(void *s, int x0, int y0, int x1, int y1, int rgb);
void vs_draw_text(void *s, int x, int y, const char *str, int rgb);
/* copy a srcw*srch buffer of 0x00RRGGBB pixels (row-major, pixels[y*srcw+x])
   into the backbuffer, nearest-neighbor scaled to the dst rect. present() after. */
void vs_blit(void *s, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth);
/* copy an unscaled sub-rect out of a 0x00RRGGBB buffer with an explicit row
   stride (in pixels, not bytes) into the backbuffer at (dstx, dsty). The
   partial-present path: blit only the dirty rect out of a full-frame canvas. */
void vs_blit_rect(void *s, const int *pixels, int stride_px,
                  int srcx, int srcy, int w, int h, int dstx, int dsty);
void vs_present(void *s); /* blit backbuffer to the window and flush */
/* present only the given backbuffer rect (clamped); cheaper than a full
   present for dirty-region repaints */
void vs_present_rect(void *s, int x, int y, int w, int h);

/* Rectangular clip for the DRAW calls (fill/rect/line/text). Set replaces
   any previous clip; clear removes it. blit/present are never clipped.
   The caller (SurfacePainter) maintains the push/pop stack. */
void vs_clip_set(void *s, int x, int y, int w, int h);
void vs_clip_clear(void *s);

int vs_text_width(void *s, const char *str);
int vs_font_ascent(void *s);
int vs_font_height(void *s);

int vs_wait(void *s);              /* blocks until an event; returns VS_EV_* */
int vs_poll(void *s);              /* non-blocking; VS_EV_NONE if none queued */
int vs_wait_timeout(void *s, int ms); /* blocks <= ms; VS_EV_TIMER on timeout */
int vs_key(void);         /* last VS_EV_KEY code */
const char *vs_text(void);/* UTF-8 text of last key ("" if none) */
int vs_mods(void);        /* VS_MOD_* bitmask at the last delivered event */

/* monotonic milliseconds — the animation/game clock (not wall time) */
long long vs_now_ms(void);

/* UI scale factor ×100 (100 = 96dpi baseline, 200 = HiDPI 2x). From
   $VOLT_SCALE when set, else Xft.dpi / the Windows DPI, else 100. */
int vs_scale_pct(void);
int vs_x(void);
int vs_y(void);
int vs_wheel(void);       /* last VS_EV_MOUSE_WHEEL delta (positive = down) */

/* escape hatches: native handles for the "drop one layer" case */
void *vs_native_display(void *s);
unsigned long vs_native_window(void *s);
void *vs_native_gc(void *s);

#endif
