/* xsend — minimal X11 event injector for driving the VoltTodo app in tests
 * (this box has no xdotool). Sends synthetic events with XSendEvent.
 *
 *   xsend <window-name> type "hello world"
 *   xsend <window-name> key Return|Escape|Delete|Up|Down|BackSpace
 *   xsend <window-name> click <x> <y>
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Window find_named(Display *dpy, Window root, const char *name) {
    char *wname = NULL;
    if (XFetchName(dpy, root, &wname) && wname) {
        int hit = strcmp(wname, name) == 0;
        XFree(wname);
        if (hit) return root;
    }
    Window parent, *kids = NULL;
    unsigned n = 0;
    Window found = 0;
    if (XQueryTree(dpy, root, &root, &parent, &kids, &n)) {
        for (unsigned i = 0; i < n && !found; i++) found = find_named(dpy, kids[i], name);
        if (kids) XFree(kids);
    }
    return found;
}

static void send_key(Display *dpy, Window win, KeySym ks, unsigned state) {
    XKeyEvent e = {0};
    e.type = KeyPress;
    e.display = dpy;
    e.window = win;
    e.root = DefaultRootWindow(dpy);
    e.subwindow = None;
    e.time = CurrentTime;
    e.same_screen = True;
    e.keycode = XKeysymToKeycode(dpy, ks);
    e.state = state;
    XSendEvent(dpy, win, True, KeyPressMask, (XEvent *)&e);
    XFlush(dpy);
    usleep(20000);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: xsend <window> type|key|click ...\n"); return 2; }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    Window win = find_named(dpy, DefaultRootWindow(dpy), argv[1]);
    if (!win) { fprintf(stderr, "window '%s' not found\n", argv[1]); return 1; }

    if (strcmp(argv[2], "type") == 0 && argc > 3) {
        for (const char *p = argv[3]; *p; p++) {
            if (*p == ' ') { send_key(dpy, win, XK_space, 0); continue; }
            KeySym ks = (KeySym)(unsigned char)*p;
            int upper = *p >= 'A' && *p <= 'Z';
            send_key(dpy, win, upper ? ks + 32 : ks, upper ? ShiftMask : 0);
        }
    } else if (strcmp(argv[2], "key") == 0 && argc > 3) {
        KeySym ks = XStringToKeysym(argv[3]);
        if (ks == NoSymbol) { fprintf(stderr, "bad keysym %s\n", argv[3]); return 1; }
        send_key(dpy, win, ks, 0);
    } else if (strcmp(argv[2], "click") == 0 && argc > 4) {
        XButtonEvent e = {0};
        e.type = ButtonPress;
        e.display = dpy;
        e.window = win;
        e.root = DefaultRootWindow(dpy);
        e.time = CurrentTime;
        e.same_screen = True;
        e.button = Button1;
        e.x = atoi(argv[3]);
        e.y = atoi(argv[4]);
        XSendEvent(dpy, win, True, ButtonPressMask, (XEvent *)&e);
        XFlush(dpy);
        usleep(20000);
    } else {
        fprintf(stderr, "bad command\n");
        return 2;
    }
    XCloseDisplay(dpy);
    return 0;
}
