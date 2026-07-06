/* xshim — the part of X11 that voltbind cannot express: the XEvent union,
 * macros, and font handling. Everything else the app calls straight from
 * the voltbind-generated Xlib binding. */
#ifndef XSHIM_H
#define XSHIM_H

/* event classes returned by xs_wait */
enum { EV_NONE, EV_EXPOSE = 1, EV_KEY = 2, EV_CLICK = 3, EV_CLOSE = 4 };

/* simplified key codes from xs_key: printable ASCII, or one of these */
enum {
    KEY_ENTER = 1000,
    KEY_BACKSPACE = 1001,
    KEY_ESC = 1002,
    KEY_UP = 1003,
    KEY_DOWN = 1004,
    KEY_DELETE = 1005,
    KEY_TAB = 1006
};

unsigned long xs_root(void *dpy);
unsigned long xs_black(void *dpy);
unsigned long xs_white(void *dpy);
unsigned long xs_rgb(void *dpy, int r, int g, int b);

/* GC with a loaded fixed-width font; also selects input + WM close events */
void *xs_gc(void *dpy, unsigned long win);
void xs_select(void *dpy, unsigned long win);
int xs_text_width(const char *s);
int xs_font_ascent(void);

int xs_wait(void *dpy);      /* blocks; returns EV_* */
int xs_key(void);            /* last EV_KEY code */
const char *xs_text(void);   /* printable text of last key ("" if none) */
int xs_x(void);
int xs_y(void);

#endif
