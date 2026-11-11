/* xcursor_sig: print a signature of the X pointer image currently shown.
 *
 *   xcursor_sig            -> "WxH hotX,hotY fnv=XXXXXXXX"
 *
 * Used by tests/run_tests_wine.sh to check that a Vyto window running under
 * Wine really changes the on-screen pointer (arrow vs I-beam vs hand). Wine
 * draws Windows cursors as unnamed X cursors, so the image itself is the only
 * thing to compare: two hovers showing the same shape give the same line.
 * Build: cc -O2 -o xcursor_sig xcursor_sig.c -lXfixes -lX11 */
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xcursor_sig: no display\n"); return 2; }
    XFixesCursorImage *ci = XFixesGetCursorImage(d);
    if (!ci) { fprintf(stderr, "xcursor_sig: no cursor image\n"); return 2; }
    unsigned h = 2166136261u;
    for (unsigned long i = 0; i < (unsigned long)ci->width * ci->height; i++) {
        unsigned long p = ci->pixels[i] & 0xffffffffUL;  /* longs, 32 bits used */
        for (int b = 0; b < 4; b++) { h ^= (p >> (8 * b)) & 0xff; h *= 16777619u; }
    }
    printf("%ux%u %u,%u fnv=%08x\n", ci->width, ci->height, ci->xhot, ci->yhot, h);
    XFree(ci);
    XCloseDisplay(d);
    return 0;
}
