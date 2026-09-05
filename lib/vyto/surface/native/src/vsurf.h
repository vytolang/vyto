/* vsurf — Layer-0 surface shim for vyto/surface: a pixel canvas plus a
 * classified event queue. X11 backend today; the API is backend-neutral so
 * SDL (or others) can slot in behind the same calls.
 *
 * Headless test backend: with VS_HEADLESS=1 in the environment, vs_open
 * connects to nothing, drawing is a no-op, metrics are fixed (9x15-like),
 * and vs_wait replays events scripted in the file named by VS_EVENTS.
 *
 * Linux framebuffer backend: with VS_FBDEV=/dev/fb0 in the environment,
 * vs_open maps the framebuffer instead of opening a display: drawing runs
 * a small software rasterizer (built-in 8x8 font), input comes from evdev
 * (/dev/input/event*), and the window size is the screen size. Pointing
 * VS_FBDEV at a regular file (with VS_FB_W/VS_FB_H) renders into that file
 * — the test path, and the embedded-bringup path. */
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
    VS_EV_MOUSE_RDOWN = 11, /* right button pressed (vs_mouse_x/y() give position) */
    VS_EV_VSYNC = 12        /* display is about to scan out; present now (vs_set_vsync) */
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
/* Refuse to let the window manager size the window below w x h. Without it a
   WM can drag a window to near-zero, and a layout pass then computes negative
   rects from a width smaller than its fixed children. X11 sets PMinSize,
   Win32 answers WM_GETMINMAXINFO; fbdev, headless and Android ignore it (their
   size is not user-driven). Values < 1 are ignored. */
void vs_set_min_size(void *s, int w, int h);

/* Drop the window manager's titlebar and border. A chromeless window is the
   point of a custom-chrome app — but the WM's move, resize, snap and close all
   live in the decorations it just gave up, so an undecorated window must
   provide them itself — that is what vs_drag_start below is for.
   X11 asks via _MOTIF_WM_HINTS, Win32 swaps WS_OVERLAPPEDWINDOW for WS_POPUP.
   fbdev, headless and Android have no window manager and ignore it. Call it
   before the first present. */
void vs_set_decorated(void *s, int on);

/* Where the window's top-left corner sits on screen, and how to move it.
   Meaningful for a chromeless window doing its own dragging; also how a future
   multi-monitor placement API positions a window. */
void vs_get_position(void *s, int *x, int *y);
void vs_move(void *s, int x, int y);

/* Hand an in-progress drag to the window manager: the app sees a mouse-down in
   its own title area and calls this, and the WM takes over until the button is
   released.
 *
 * Preferred over moving the window yourself on every motion event, because the
 * WM applies its own snapping, edge-tiling and multi-monitor behaviour — a
 * hand-rolled drag gets none of that and fights the compositor for pacing.
 * Returns 1 when the WM took it, 0 when it did not (no _NET_WM_MOVERESIZE, or
 * a backend without one), in which case fall back to vs_move on motion.
 *
 * `edge` selects the operation, matching the _NET_WM_MOVERESIZE values:
 * 0..7 are the eight resize edges (see VS_EDGE_*), 8 is a move. */
int vs_drag_start(void *s, int edge, int x_root, int y_root);

/* Resize the window. Pairs with vs_move for placing a kiosk or signage window:
   create, size to the target screen, move onto it. Values < 1 are ignored. */
void vs_set_size(void *s, int w, int h);

/* Fullscreen the window on the monitor it currently occupies (X11 asks the WM
   via _NET_WM_STATE_FULLSCREEN; Win32 goes borderless at the monitor rect).
   To choose WHICH monitor, move the window onto it first and then fullscreen —
   the WM fullscreens where the window is. Returns 1 when the request went out,
   0 where there is no window manager to ask. */
int vs_set_fullscreen(void *s, int on);

/* Attached monitors, for placing a window on a chosen screen.
 *
 * Fills up to `max` entries and returns how many exist — call with max 0 to
 * count first. Geometry is in the same coordinate space vs_move uses, so
 * placing a window at a monitor's x/y puts it on that monitor.
 *
 * X11 reads XRandR, loaded with dlopen rather than linked: monitor enumeration
 * should not put libXrandr on the link line of every GUI program, and a machine
 * without it still runs — it just reports the one screen it can see. Win32 uses
 * EnumDisplayMonitors. Backends with no window system report a single monitor
 * the size of their surface. */
typedef struct VsMonitor {
    int x, y, w, h;
    int primary;     /* 1 for the primary monitor */
    int scale_pct;   /* UI scale x100, 100 = 96dpi; 0 when unknown */
} VsMonitor;
int vs_monitors(VsMonitor *out, int max);

/* Read a window's pixels back, as 0x00RRGGBB rows of `*w` pixels.
 *
 * `win` is a NATIVE window handle — vs_native_window() for one of ours, or an
 * XID / HWND obtained elsewhere for somebody else's. It is not a VSurf: the
 * interesting case is a window this process did not create, and a surface
 * handle cannot name one.
 *
 * Writes at most `cap` pixels into `out` and reports the window's size through
 * `*w`/`*h`. Returns the number of pixels the window HAS, so a caller can pass
 * cap 0 to size a buffer and call again — a return larger than `cap` means the
 * capture was truncated, not that it failed. 0 means it could not be read.
 *
 * Only the platforms whose window systems allow a client to read another
 * window: X11 (XGetImage) and Win32 (BitBlt from the window DC). It returns 0
 * everywhere else, and that includes **Wayland**, where capture is deliberately
 * behind a portal with user consent rather than a function call — treat 0 as
 * the normal answer on Linux desktops rather than an error, because it will be
 * once Wayland is the common case.
 *
 * Two limits that are properties of X11, not of this call:
 *   - Without a compositing manager the server keeps no offscreen storage for
 *     an obscured window, so a covered region reads back as whatever is drawn
 *     on top of it. With a compositor (the usual desktop) the content is
 *     preserved and the capture is correct. There is no way to tell from here
 *     which you got.
 *   - An unmapped or iconified window has no content to read at all.
 *
 * For previewing a window this process renders, do not use this: blit the
 * canvas you already have (see apps/multiwindow/videowall.vt). Capture exists
 * for windows you did not draw. */
int vs_capture(unsigned long win, int *out, int cap, int *w, int *h);

/* The `edge` values for vs_drag_start, matching _NET_WM_MOVERESIZE. */
enum {
    VS_EDGE_TOPLEFT = 0,
    VS_EDGE_TOP = 1,
    VS_EDGE_TOPRIGHT = 2,
    VS_EDGE_RIGHT = 3,
    VS_EDGE_BOTTOMRIGHT = 4,
    VS_EDGE_BOTTOM = 5,
    VS_EDGE_BOTTOMLEFT = 6,
    VS_EDGE_LEFT = 7,
    VS_EDGE_MOVE = 8
};

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
/* Ask the backend to deliver VS_EV_VSYNC once per display refresh while `on`.
 * Returns 1 when the backend really drives a vsync clock, 0 when it does not —
 * a caller that gates presentation on VS_EV_VSYNC must present eagerly instead
 * when this returns 0, or it will never present at all. Only the Android arm
 * returns 1 today (Choreographer); X11/Win32/fbdev/headless return 0. */
int vs_set_vsync(void *s, int on);
/* The region the last VS_EV_EXPOSE actually lost, as the union of that Expose
   run. Returns 1 and fills *out when the backend reported one, 0 when it did
   not — repaint the whole window in that case. Reading it CONSUMES it, so call
   once per expose. X11 reports a real region; Win32, fbdev, headless and
   Android always return 0. */
typedef struct VsRect { int x, y, w, h; } VsRect;
int vs_damage(void *s, VsRect *out);

int vs_key(void *s);         /* last VS_EV_KEY code */
const char *vs_text(void *s);/* UTF-8 text of last key ("" if none) */
int vs_mods(void *s);        /* VS_MOD_* bitmask at the last delivered event */

/* monotonic milliseconds — the animation/game clock (not wall time) */
long long vs_now_ms(void);

/* UI scale factor ×100 (100 = 96dpi baseline, 200 = HiDPI 2x). From
   $VYTO_SCALE when set, else Xft.dpi / the Windows DPI, else 100. */
int vs_scale_pct(void);
int vs_x(void *s);
int vs_y(void *s);
int vs_wheel(void *s);       /* last VS_EV_MOUSE_WHEEL delta (positive = down) */

/* Clipboard, UTF-8 text. set copies the text out; get returns "" when the
   clipboard is empty or unavailable, and the returned pointer stays valid
   until the next clipboard call. X11 speaks the CLIPBOARD selection (get
   waits up to ~300ms for the owner); Win32 uses CF_UNICODETEXT; headless
   and framebuffer keep a process-local buffer, seedable from the event
   script with "clip <text>". */
void vs_clipboard_set(void *s, const char *text);
const char *vs_clipboard_get(void *s);

/* The descriptor this surface's events arrive on, or -1 where the backend has
   none. Lets an external event loop (vyto/os/reactor) wait on a window and a
   set of sockets in ONE blocking call instead of polling the window on a timer.

   X11 returns the display connection socket. Win32 (-1) blocks on a thread
   message queue and Android (-1) on a pthread condvar; neither is a descriptor,
   and callers fall back to a bounded vs_wait_timeout there.

   Readable does NOT mean an event is ready: Xlib buffers, so check
   vs_events_pending() before blocking, not only after the fd fires. */
int vs_event_fd(void *s);

/* Events already decoded and waiting, independent of the descriptor. Xlib's
   queue can be non-empty with nothing readable on the connection, so a loop
   that blocked purely on the fd would sleep holding events. */
int vs_events_pending(void *s);

/* Block up to ms for a window event OR readability on any of `n` caller fds.
   Returns a VS_EV_* exactly as vs_wait_timeout does; VS_EV_TIMER covers both
   "the timeout elapsed" and "one of your fds is ready", so check your own
   descriptors on every return.

   Lets a GUI app waiting on work that finishes elsewhere — a forked worker, a
   socket — block properly instead of waking on a timer to ask. X11 folds the
   fds into the select it already performs; Win32 and Android cannot (a C
   runtime fd is not a WaitForMultipleObjects handle, and a condvar is not
   selectable) and degrade to the plain bounded wait, which is the same polling
   the caller would have done by hand.

   fds==NULL or n<=0 is exactly vs_wait_timeout. */
int vs_wait_timeout_fds(void *s, int ms, const int *fds, int n);

/* Whether vs_wait_timeout_fds really waits on the caller's fds (1) or degrades
   to a bounded poll (0). Callers use it to decide whether they may block
   indefinitely or must keep a timer ceiling. */
int vs_can_wait_fds(void *s);

/* escape hatches: native handles for the "drop one layer" case */
void *vs_native_display(void *s);
unsigned long vs_native_window(void *s);
void *vs_native_gc(void *s);

#endif
