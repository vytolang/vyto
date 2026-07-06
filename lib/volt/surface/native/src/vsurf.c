#include "vsurf.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VSurf {
    Display *dpy; /* NULL in headless mode */
    Window win;
    Pixmap back;
    GC gc;
    int w, h;
} VSurf;

static XFontStruct *font;
static Atom wm_delete;
static int last_key, last_x, last_y;
static char last_text[32];

/* ---- headless backend state ---- */

static int headless_on(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("VS_HEADLESS");
        cached = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static FILE *script;        /* $VS_EVENTS */
static char script_line[512];
static const char *type_p;  /* rest of a "type ..." line being replayed */

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

/* ---- lifecycle ---- */

void *vs_open(const char *title, int w, int h) {
    VSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    if (headless_on()) {
        const char *ev = getenv("VS_EVENTS");
        if (ev && !script) script = fopen(ev, "r");
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
    s->dpy = dpy;
    s->win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)w, (unsigned)h, 1,
                                 BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, s->win, title);
    XSelectInput(dpy, s->win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                                  StructureNotifyMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, s->win, &wm_delete, 1);
    s->gc = XCreateGC(dpy, s->win, 0, 0);
    if (!font) font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, s->gc, font->fid);
    s->back = XCreatePixmap(dpy, s->win, (unsigned)w, (unsigned)h, (unsigned)depth);
    XMapWindow(dpy, s->win);
    return s;
}

void vs_close(void *vs) {
    VSurf *s = vs;
    if (!s) return;
    if (s->dpy) {
        XFreePixmap(s->dpy, s->back);
        XFreeGC(s->dpy, s->gc);
        XDestroyWindow(s->dpy, s->win);
        XCloseDisplay(s->dpy);
    }
    free(s);
}

int vs_width(void *vs) { return ((VSurf *)vs)->w; }
int vs_height(void *vs) { return ((VSurf *)vs)->h; }

void vs_set_title(void *vs, const char *t) {
    VSurf *s = vs;
    if (s->dpy) XStoreName(s->dpy, s->win, t);
}

/* ---- drawing (into the backbuffer) ---- */

void vs_fill_rect(void *vs, int x, int y, int w, int h, int rgb) {
    VSurf *s = vs;
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XFillRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)w, (unsigned)h);
}

void vs_draw_rect(void *vs, int x, int y, int w, int h, int rgb) {
    VSurf *s = vs;
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XDrawRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

void vs_draw_line(void *vs, int x0, int y0, int x1, int y1, int rgb) {
    VSurf *s = vs;
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XDrawLine(s->dpy, s->back, s->gc, x0, y0, x1, y1);
}

void vs_draw_text(void *vs, int x, int y, const char *str, int rgb) {
    VSurf *s = vs;
    if (!s->dpy) return;
    XSetForeground(s->dpy, s->gc, pixel_of(s->dpy, rgb));
    XDrawString(s->dpy, s->back, s->gc, x, y, str, (int)strlen(str));
}

void vs_present(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return;
    XCopyArea(s->dpy, s->back, s->win, s->gc, 0, 0, (unsigned)s->w, (unsigned)s->h, 0, 0);
    XFlush(s->dpy);
}

/* ---- text metrics (fixed 9x15-like numbers in headless mode) ---- */

int vs_text_width(void *vs, const char *str) {
    VSurf *s = vs;
    if (!s->dpy || !font) return 9 * (int)strlen(str);
    return XTextWidth(font, str, (int)strlen(str));
}

int vs_font_ascent(void *vs) {
    VSurf *s = vs;
    if (!s->dpy || !font) return 11;
    return font->ascent;
}

int vs_font_height(void *vs) {
    VSurf *s = vs;
    if (!s->dpy || !font) return 15;
    return font->ascent + font->descent;
}

/* ---- headless event script ----
 * one directive per line:
 *   type <text>      each character becomes a key event
 *   key <name>       Enter Backspace Esc Up Down Left Right Delete Tab Home End
 *   click X Y        mouse down (then up) at X,Y
 *   resize W H
 *   expose
 *   close
 * '#' starts a comment; EOF acts as close. */

static int headless_wait(VSurf *s) {
    static int pending_up; /* deliver MOUSE_UP after each click's MOUSE_DOWN */
    if (pending_up) {
        pending_up = 0;
        return VS_EV_MOUSE_UP;
    }
    for (;;) {
        if (type_p && *type_p) {
            last_key = (unsigned char)*type_p;
            last_text[0] = *type_p;
            last_text[1] = 0;
            type_p++;
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
        if (strncmp(script_line, "key ", 4) == 0) {
            const char *k = script_line + 4;
            last_text[0] = 0;
            if (strcmp(k, "Enter") == 0) last_key = VS_KEY_ENTER;
            else if (strcmp(k, "Backspace") == 0) last_key = VS_KEY_BACKSPACE;
            else if (strcmp(k, "Esc") == 0) last_key = VS_KEY_ESC;
            else if (strcmp(k, "Up") == 0) last_key = VS_KEY_UP;
            else if (strcmp(k, "Down") == 0) last_key = VS_KEY_DOWN;
            else if (strcmp(k, "Left") == 0) last_key = VS_KEY_LEFT;
            else if (strcmp(k, "Right") == 0) last_key = VS_KEY_RIGHT;
            else if (strcmp(k, "Delete") == 0) last_key = VS_KEY_DELETE;
            else if (strcmp(k, "Tab") == 0) last_key = VS_KEY_TAB;
            else if (strcmp(k, "Home") == 0) last_key = VS_KEY_HOME;
            else if (strcmp(k, "End") == 0) last_key = VS_KEY_END;
            else continue;
            return VS_EV_KEY;
        }
        if (sscanf(script_line, "click %d %d", &last_x, &last_y) == 2) {
            pending_up = 1;
            return VS_EV_MOUSE_DOWN;
        }
        {
            int w, h;
            if (sscanf(script_line, "resize %d %d", &w, &h) == 2) {
                s->w = w;
                s->h = h;
                return VS_EV_RESIZE;
            }
        }
        if (strcmp(script_line, "expose") == 0) return VS_EV_EXPOSE;
        if (strcmp(script_line, "close") == 0) return VS_EV_CLOSE;
        /* unknown directive: skip */
    }
}

/* ---- event pump ---- */

int vs_wait(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return headless_wait(s);
    XEvent e;
    for (;;) {
        XNextEvent(s->dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0) return VS_EV_EXPOSE;
            break;
        case ConfigureNotify: {
            int w = e.xconfigure.width, h = e.xconfigure.height;
            if (w != s->w || h != s->h) {
                s->w = w;
                s->h = h;
                XFreePixmap(s->dpy, s->back);
                s->back = XCreatePixmap(s->dpy, s->win, (unsigned)w, (unsigned)h,
                                        (unsigned)DefaultDepth(s->dpy, DefaultScreen(s->dpy)));
                return VS_EV_RESIZE;
            }
            break;
        }
        case KeyPress: {
            char buf[16];
            KeySym ks = 0;
            int n = XLookupString(&e.xkey, buf, sizeof buf - 1, &ks, NULL);
            last_text[0] = 0;
            switch (ks) {
            case XK_Return: case XK_KP_Enter: last_key = VS_KEY_ENTER; return VS_EV_KEY;
            case XK_BackSpace: last_key = VS_KEY_BACKSPACE; return VS_EV_KEY;
            case XK_Escape: last_key = VS_KEY_ESC; return VS_EV_KEY;
            case XK_Up: last_key = VS_KEY_UP; return VS_EV_KEY;
            case XK_Down: last_key = VS_KEY_DOWN; return VS_EV_KEY;
            case XK_Left: last_key = VS_KEY_LEFT; return VS_EV_KEY;
            case XK_Right: last_key = VS_KEY_RIGHT; return VS_EV_KEY;
            case XK_Delete: last_key = VS_KEY_DELETE; return VS_EV_KEY;
            case XK_Tab: last_key = VS_KEY_TAB; return VS_EV_KEY;
            case XK_Home: last_key = VS_KEY_HOME; return VS_EV_KEY;
            case XK_End: last_key = VS_KEY_END; return VS_EV_KEY;
            default:
                if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
                    last_key = buf[0];
                    last_text[0] = buf[0];
                    last_text[1] = 0;
                    return VS_EV_KEY;
                }
            }
            break; /* modifier or unmapped key: keep waiting */
        }
        case ButtonPress:
            if (e.xbutton.button == Button1) {
                last_x = e.xbutton.x;
                last_y = e.xbutton.y;
                return VS_EV_MOUSE_DOWN;
            }
            break;
        case ButtonRelease:
            if (e.xbutton.button == Button1) {
                last_x = e.xbutton.x;
                last_y = e.xbutton.y;
                return VS_EV_MOUSE_UP;
            }
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == wm_delete) return VS_EV_CLOSE;
            break;
        default:
            break;
        }
    }
}

int vs_key(void) { return last_key; }
const char *vs_text(void) { return last_text; }
int vs_x(void) { return last_x; }
int vs_y(void) { return last_y; }

/* ---- escape hatches ---- */

void *vs_native_display(void *vs) { return ((VSurf *)vs)->dpy; }
unsigned long vs_native_window(void *vs) { return (unsigned long)((VSurf *)vs)->win; }
void *vs_native_gc(void *vs) { return ((VSurf *)vs)->gc; }
