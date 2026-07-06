#include "xshim.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <string.h>

static XFontStruct *font;
static Atom wm_delete;
static int last_key, last_x, last_y;
static char last_text[32];

unsigned long xs_root(void *dpy) { return DefaultRootWindow((Display *)dpy); }
unsigned long xs_black(void *dpy) { return BlackPixel((Display *)dpy, DefaultScreen((Display *)dpy)); }
unsigned long xs_white(void *dpy) { return WhitePixel((Display *)dpy, DefaultScreen((Display *)dpy)); }

unsigned long xs_rgb(void *vdpy, int r, int g, int b) {
    Display *dpy = vdpy;
    XColor c;
    c.red = (unsigned short)(r * 257);
    c.green = (unsigned short)(g * 257);
    c.blue = (unsigned short)(b * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c))
        return xs_black(vdpy);
    return c.pixel;
}

void *xs_gc(void *vdpy, unsigned long win) {
    Display *dpy = vdpy;
    GC gc = XCreateGC(dpy, (Drawable)win, 0, 0);
    if (!font) font = XLoadQueryFont(dpy, "9x15");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);
    return gc;
}

void xs_select(void *vdpy, unsigned long win) {
    Display *dpy = vdpy;
    XSelectInput(dpy, (Window)win, ExposureMask | KeyPressMask | ButtonPressMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, (Window)win, &wm_delete, 1);
}

int xs_text_width(const char *s) {
    if (!font) return 9 * (int)strlen(s);
    return XTextWidth(font, s, (int)strlen(s));
}

int xs_font_ascent(void) { return font ? font->ascent : 11; }

int xs_wait(void *vdpy) {
    Display *dpy = vdpy;
    XEvent e;
    for (;;) {
        XNextEvent(dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0) return EV_EXPOSE;
            break;
        case KeyPress: {
            char buf[16];
            KeySym ks = 0;
            int n = XLookupString(&e.xkey, buf, sizeof buf - 1, &ks, NULL);
            last_text[0] = 0;
            switch (ks) {
            case XK_Return: case XK_KP_Enter: last_key = KEY_ENTER; return EV_KEY;
            case XK_BackSpace: last_key = KEY_BACKSPACE; return EV_KEY;
            case XK_Escape: last_key = KEY_ESC; return EV_KEY;
            case XK_Up: last_key = KEY_UP; return EV_KEY;
            case XK_Down: last_key = KEY_DOWN; return EV_KEY;
            case XK_Delete: last_key = KEY_DELETE; return EV_KEY;
            case XK_Tab: last_key = KEY_TAB; return EV_KEY;
            default:
                if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
                    last_key = buf[0];
                    last_text[0] = buf[0];
                    last_text[1] = 0;
                    return EV_KEY;
                }
            }
            break; /* modifier or unmapped key: keep waiting */
        }
        case ButtonPress:
            if (e.xbutton.button == Button1) {
                last_x = e.xbutton.x;
                last_y = e.xbutton.y;
                return EV_CLICK;
            }
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == wm_delete) return EV_CLOSE;
            break;
        default:
            break;
        }
    }
}

int xs_key(void) { return last_key; }
const char *xs_text(void) { return last_text; }
int xs_x(void) { return last_x; }
int xs_y(void) { return last_y; }
