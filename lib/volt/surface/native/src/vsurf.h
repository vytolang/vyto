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
    VS_EV_MOUSE_MOVE = 7 /* reserved; not delivered in v1 */
};

/* simplified key codes from vs_key: printable ASCII, or one of these */
enum {
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
    VS_KEY_END = 1010
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
void vs_present(void *s); /* blit backbuffer to the window and flush */

int vs_text_width(void *s, const char *str);
int vs_font_ascent(void *s);
int vs_font_height(void *s);

int vs_wait(void *s);     /* blocks; returns VS_EV_* */
int vs_key(void);         /* last VS_EV_KEY code */
const char *vs_text(void);/* printable text of last key ("" if none) */
int vs_x(void);
int vs_y(void);

/* escape hatches: native handles for the "drop one layer" case */
void *vs_native_display(void *s);
unsigned long vs_native_window(void *s);
void *vs_native_gc(void *s);

#endif
