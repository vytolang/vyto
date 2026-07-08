/* vsurf backends:
 *   - Win32 GDI when compiling for Windows (_WIN32)
 *   - X11 everywhere else
 *   - headless (VS_HEADLESS=1): no window system at all; events replay from
 *     the $VS_EVENTS script. Shared by every platform, used by the tests.
 */
#include "vsurf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int last_key, last_x, last_y, last_wheel;
static char last_text[32];

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
 *   resize W H
 *   expose
 *   tick             a timer tick (VS_EV_TIMER), for game-loop tests
 *   close
 * '#' starts a comment; EOF acts as close. */
static int headless_wait(int *w, int *h) {
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
        if (strncmp(script_line, "key ", 4) == 0 || strncmp(script_line, "keyup ", 6) == 0) {
            int up = script_line[3] == 'u';
            const char *k = script_line + (up ? 6 : 4);
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
            return up ? VS_EV_KEY_UP : VS_EV_KEY;
        }
        if (sscanf(script_line, "click %d %d", &last_x, &last_y) == 2) {
            pending_up = 1;
            return VS_EV_MOUSE_DOWN;
        }
        {
            int nw, nh;
            if (sscanf(script_line, "resize %d %d", &nw, &nh) == 2) {
                *w = nw;
                *h = nh;
                return VS_EV_RESIZE;
            }
        }
        if (strcmp(script_line, "expose") == 0) return VS_EV_EXPOSE;
        if (strcmp(script_line, "tick") == 0) return VS_EV_TIMER;
        if (strcmp(script_line, "close") == 0) return VS_EV_CLOSE;
        {
            int sc, sx, sy;
            if (sscanf(script_line, "scroll %d %d %d", &sc, &sx, &sy) == 3) {
                last_wheel = sc;
                last_x = sx;
                last_y = sy;
                return VS_EV_MOUSE_WHEEL;
            }
            if (sscanf(script_line, "scroll %d", &sc) == 1) {
                last_wheel = sc;
                return VS_EV_MOUSE_WHEEL;
            }
        }
        /* unknown directive: skip */
    }
}

int vs_key(void) { return last_key; }
const char *vs_text(void) { return last_text; }
int vs_x(void) { return last_x; }
int vs_y(void) { return last_y; }
int vs_wheel(void) { return last_wheel; }

#ifdef _WIN32
/* ================================================================ Win32 */

#include <windows.h>

typedef struct VSurf {
    HWND hwnd; /* NULL in headless mode */
    HDC memdc; /* backbuffer */
    HBITMAP bmp;
    HBITMAP bmp0; /* original 1x1 bitmap of memdc, restored before delete */
    HFONT font;
    int w, h;
} VSurf;

/* WndProc -> vs_wait event queue (single window; small ring buffer) */
typedef struct QEv {
    int type, key, x, y;
    char text[2];
} QEv;
static QEv evq[64];
static int evq_head, evq_len;

static void q_push(int type, int key, int x, int y, int ch) {
    if (evq_len == 64) return; /* drop when full; the loop catches up */
    QEv *e = &evq[(evq_head + evq_len) % 64];
    e->type = type;
    e->key = key;
    e->x = x;
    e->y = y;
    e->text[0] = (char)ch;
    e->text[1] = 0;
    evq_len++;
}

/* 0xRRGGBB -> COLORREF (0x00BBGGRR) */
static COLORREF cref(int rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static void make_backbuffer(VSurf *s) {
    HDC wdc = GetDC(s->hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, s->w > 0 ? s->w : 1, s->h > 0 ? s->h : 1);
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
    VSurf *s = (VSurf *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (s && s->memdc) BitBlt(dc, 0, 0, s->w, s->h, s->memdc, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        q_push(VS_EV_EXPOSE, 0, 0, 0, 0);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (s && w > 0 && h > 0 && (w != s->w || h != s->h)) {
            s->w = w;
            s->h = h;
            make_backbuffer(s);
            q_push(VS_EV_RESIZE, 0, 0, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        q_push(VS_EV_CLOSE, 0, 0, 0, 0);
        return 0; /* the Volt loop decides; vs_close destroys the window */
    case WM_LBUTTONDOWN:
        q_push(VS_EV_MOUSE_DOWN, 0, (short)LOWORD(lp), (short)HIWORD(lp), 0);
        return 0;
    case WM_LBUTTONUP:
        q_push(VS_EV_MOUSE_UP, 0, (short)LOWORD(lp), (short)HIWORD(lp), 0);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        /* normalize: one wheel notch = WHEEL_DELTA (120); map to +/-3 lines */
        int lines = (delta * 3) / 120;
        q_push(VS_EV_MOUSE_WHEEL, lines, pt.x, pt.y, 0);
        return 0;
    }
    case WM_KEYDOWN: {
        int k = 0;
        switch (wp) {
        case VK_RETURN: k = VS_KEY_ENTER; break;
        case VK_BACK: k = VS_KEY_BACKSPACE; break;
        case VK_ESCAPE: k = VS_KEY_ESC; break;
        case VK_UP: k = VS_KEY_UP; break;
        case VK_DOWN: k = VS_KEY_DOWN; break;
        case VK_LEFT: k = VS_KEY_LEFT; break;
        case VK_RIGHT: k = VS_KEY_RIGHT; break;
        case VK_DELETE: k = VS_KEY_DELETE; break;
        case VK_TAB: k = VS_KEY_TAB; break;
        case VK_HOME: k = VS_KEY_HOME; break;
        case VK_END: k = VS_KEY_END; break;
        }
        if (k) {
            q_push(VS_EV_KEY, k, 0, 0, 0);
            return 0;
        }
        break; /* DefWindowProc; TranslateMessage yields WM_CHAR */
    }
    case WM_KEYUP: {
        int k = 0;
        switch (wp) {
        case VK_RETURN: k = VS_KEY_ENTER; break;
        case VK_BACK: k = VS_KEY_BACKSPACE; break;
        case VK_ESCAPE: k = VS_KEY_ESC; break;
        case VK_UP: k = VS_KEY_UP; break;
        case VK_DOWN: k = VS_KEY_DOWN; break;
        case VK_LEFT: k = VS_KEY_LEFT; break;
        case VK_RIGHT: k = VS_KEY_RIGHT; break;
        case VK_DELETE: k = VS_KEY_DELETE; break;
        case VK_TAB: k = VS_KEY_TAB; break;
        case VK_HOME: k = VS_KEY_HOME; break;
        case VK_END: k = VS_KEY_END; break;
        default:
            /* no WM_CHAR on release: report printable letters/digits/space by VK */
            if ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9')) k = (int)wp;
            else if (wp == VK_SPACE) k = 32;
        }
        if (k) q_push(VS_EV_KEY_UP, k, 0, 0, 0);
        return 0;
    }
    case WM_CHAR:
        if (wp >= 32 && wp < 127) q_push(VS_EV_KEY, (int)wp, 0, 0, (int)wp);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void *vs_open(const char *title, int w, int h) {
    VSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    if (headless_on()) {
        headless_open_script();
        return s;
    }
    static int registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = vs_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "VoltSurface";
        if (!RegisterClassA(&wc)) { free(s); return NULL; }
        registered = 1;
    }
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s->hwnd = CreateWindowA("VoltSurface", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                            GetModuleHandle(NULL), NULL);
    if (!s->hwnd) { free(s); return NULL; }
    SetWindowLongPtr(s->hwnd, GWLP_USERDATA, (LONG_PTR)s);
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
        SetWindowLongPtr(s->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(s->hwnd);
    }
    free(s);
}

int vs_width(void *vs) { return ((VSurf *)vs)->w; }
int vs_height(void *vs) { return ((VSurf *)vs)->h; }

void vs_set_title(void *vs, const char *t) {
    VSurf *s = vs;
    if (s->hwnd) SetWindowTextA(s->hwnd, t);
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
    TEXTMETRICA tm;
    GetTextMetricsA(s->memdc, &tm);
    SetTextColor(s->memdc, cref(rgb));
    TextOutA(s->memdc, x, y - tm.tmAscent, str, (int)strlen(str));
}

void vs_blit(void *vs, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth) {
    VSurf *s = vs;
    if (!s->hwnd || srcw <= 0 || srch <= 0) return;
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
}

void vs_present(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return;
    HDC dc = GetDC(s->hwnd);
    BitBlt(dc, 0, 0, s->w, s->h, s->memdc, 0, 0, SRCCOPY);
    ReleaseDC(s->hwnd, dc);
}

int vs_text_width(void *vs, const char *str) {
    VSurf *s = vs;
    if (!s->hwnd) return 9 * (int)strlen(str);
    SIZE sz;
    GetTextExtentPoint32A(s->memdc, str, (int)strlen(str), &sz);
    return (int)sz.cx;
}

int vs_font_ascent(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return 11;
    TEXTMETRICA tm;
    GetTextMetricsA(s->memdc, &tm);
    return (int)tm.tmAscent;
}

int vs_font_height(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return 15;
    TEXTMETRICA tm;
    GetTextMetricsA(s->memdc, &tm);
    return (int)tm.tmHeight;
}

static int q_pop(void) {
    QEv e = evq[evq_head];
    evq_head = (evq_head + 1) % 64;
    evq_len--;
    last_key = e.key;
    last_x = e.x;
    last_y = e.y;
    last_wheel = (e.type == VS_EV_MOUSE_WHEEL) ? e.key : 0;
    last_text[0] = e.text[0];
    last_text[1] = 0;
    return e.type;
}

int vs_wait(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return headless_wait(&s->w, &s->h);
    while (evq_len == 0) {
        MSG msg;
        if (GetMessageA(&msg, NULL, 0, 0) <= 0) return VS_EV_CLOSE;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return q_pop();
}

int vs_poll(void *vs) {
    VSurf *s = vs;
    if (!s->hwnd) return headless_wait(&s->w, &s->h);
    MSG msg;
    while (evq_len == 0 && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return evq_len ? q_pop() : VS_EV_NONE;
}

int vs_wait_timeout(void *vs, int ms) {
    VSurf *s = vs;
    if (!s->hwnd) return headless_wait(&s->w, &s->h);
    for (;;) {
        if (evq_len) return q_pop();
        DWORD r = MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)ms, QS_ALLINPUT);
        if (r == WAIT_TIMEOUT) return VS_EV_TIMER;
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

void *vs_native_display(void *vs) {
    (void)vs;
    return NULL; /* no display concept on Win32 */
}
unsigned long vs_native_window(void *vs) { return (unsigned long)(uintptr_t)((VSurf *)vs)->hwnd; }
void *vs_native_gc(void *vs) { return ((VSurf *)vs)->memdc; }

#else
/* ================================================================== X11 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <sys/select.h>

typedef struct VSurf {
    Display *dpy; /* NULL in headless mode */
    Window win;
    Pixmap back;
    GC gc;
    int w, h;
} VSurf;

static XFontStruct *font;
static Atom wm_delete;

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

void *vs_open(const char *title, int w, int h) {
    VSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    if (headless_on()) {
        headless_open_script();
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
    XSelectInput(dpy, s->win, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                                  ButtonReleaseMask | StructureNotifyMask);
    /* only emit KeyRelease on real release, not auto-repeat — matters for
       held-key game input (EV_KEY / EV_KEY_UP pairing) */
    XkbSetDetectableAutoRepeat(dpy, True, NULL);
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

/* nearest-neighbor blit of a 0x00RRGGBB pixel buffer into the backbuffer.
   A dest-sized XImage is cached across calls and reused (single window). */
void vs_blit(void *vs, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth) {
    VSurf *s = vs;
    if (!s->dpy || srcw <= 0 || srch <= 0 || dstw <= 0 || dsth <= 0) return;
    static XImage *img;
    static int img_w, img_h;
    int scr = DefaultScreen(s->dpy);
    if (!img || img_w != dstw || img_h != dsth) {
        if (img) XDestroyImage(img); /* frees the malloc'd data too */
        char *buf = malloc((size_t)dstw * (size_t)dsth * 4);
        if (!buf) return;
        img = XCreateImage(s->dpy, DefaultVisual(s->dpy, scr),
                           (unsigned)DefaultDepth(s->dpy, scr), ZPixmap, 0, buf,
                           (unsigned)dstw, (unsigned)dsth, 32, 0);
        if (!img) { free(buf); return; }
        img_w = dstw;
        img_h = dsth;
    }
    /* Fast path: on a little-endian 32bpp TrueColor visual the image row is a
       plain uint32 array of 0x00RRGGBB — pack directly, skipping 786K XPutPixel
       calls per frame. Falls back to XPutPixel on paletted/odd visuals. */
    if (truecolor && img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        for (int dy = 0; dy < dsth; dy++) {
            int sy = dy * srch / dsth;
            const int *src = pixels + sy * srcw;
            uint32_t *row = (uint32_t *)(img->data + dy * img->bytes_per_line);
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
                XPutPixel(img, dx, dy, pixel_of(s->dpy, pixels[sy * srcw + dx * srcw / dstw]));
        }
    }
    XPutImage(s->dpy, s->back, s->gc, img, 0, 0, dstx, dsty, (unsigned)dstw, (unsigned)dsth);
}

void vs_present(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return;
    XCopyArea(s->dpy, s->back, s->win, s->gc, 0, 0, (unsigned)s->w, (unsigned)s->h, 0, 0);
    XFlush(s->dpy);
}

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

/* decode an X key event into last_key/last_text; 1 if a key we deliver, else 0 */
static int x11_decode_key(XKeyEvent *ke) {
    char buf[16];
    KeySym ks = 0;
    int n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL);
    last_text[0] = 0;
    switch (ks) {
    case XK_Return: case XK_KP_Enter: last_key = VS_KEY_ENTER; return 1;
    case XK_BackSpace: last_key = VS_KEY_BACKSPACE; return 1;
    case XK_Escape: last_key = VS_KEY_ESC; return 1;
    case XK_Up: last_key = VS_KEY_UP; return 1;
    case XK_Down: last_key = VS_KEY_DOWN; return 1;
    case XK_Left: last_key = VS_KEY_LEFT; return 1;
    case XK_Right: last_key = VS_KEY_RIGHT; return 1;
    case XK_Delete: last_key = VS_KEY_DELETE; return 1;
    case XK_Tab: last_key = VS_KEY_TAB; return 1;
    case XK_Home: last_key = VS_KEY_HOME; return 1;
    case XK_End: last_key = VS_KEY_END; return 1;
    default:
        if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
            last_key = buf[0];
            last_text[0] = buf[0];
            last_text[1] = 0;
            return 1;
        }
    }
    return 0; /* modifier or unmapped key */
}

/* translate one XEvent into a VS_EV_* code, or -1 to ignore and keep waiting */
static int x11_translate(VSurf *s, XEvent *e) {
    switch (e->type) {
    case Expose:
        if (e->xexpose.count == 0) return VS_EV_EXPOSE;
        return -1;
    case ConfigureNotify: {
        int w = e->xconfigure.width, h = e->xconfigure.height;
        if (w != s->w || h != s->h) {
            s->w = w;
            s->h = h;
            XFreePixmap(s->dpy, s->back);
            s->back = XCreatePixmap(s->dpy, s->win, (unsigned)w, (unsigned)h,
                                    (unsigned)DefaultDepth(s->dpy, DefaultScreen(s->dpy)));
            return VS_EV_RESIZE;
        }
        return -1;
    }
    case KeyPress:
        return x11_decode_key(&e->xkey) ? VS_EV_KEY : -1;
    case KeyRelease:
        return x11_decode_key(&e->xkey) ? VS_EV_KEY_UP : -1;
    case ButtonPress:
        if (e->xbutton.button == Button1) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            return VS_EV_MOUSE_DOWN;
        }
        if (e->xbutton.button == Button4) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            last_wheel = -3; /* up = negative (content moves down) */
            return VS_EV_MOUSE_WHEEL;
        }
        if (e->xbutton.button == Button5) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            last_wheel = 3; /* down = positive (content moves up) */
            return VS_EV_MOUSE_WHEEL;
        }
        return -1;
    case ButtonRelease:
        if (e->xbutton.button == Button1) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            return VS_EV_MOUSE_UP;
        }
        return -1;
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == wm_delete) return VS_EV_CLOSE;
        return -1;
    default:
        return -1;
    }
}

int vs_wait(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return headless_wait(&s->w, &s->h);
    for (;;) {
        XEvent e;
        XNextEvent(s->dpy, &e);
        int r = x11_translate(s, &e);
        if (r >= 0) return r;
    }
}

/* non-blocking: next pending event, or VS_EV_NONE if the queue is empty.
   Flushes output first so this frame's drawing is on screen. */
int vs_poll(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return headless_wait(&s->w, &s->h);
    XFlush(s->dpy);
    while (XPending(s->dpy)) {
        XEvent e;
        XNextEvent(s->dpy, &e);
        int r = x11_translate(s, &e);
        if (r >= 0) return r;
    }
    return VS_EV_NONE;
}

/* block up to ms milliseconds for an event; VS_EV_TIMER if it elapses with
   none. The tick source for game loops. */
int vs_wait_timeout(void *vs, int ms) {
    VSurf *s = vs;
    if (!s->dpy) return headless_wait(&s->w, &s->h);
    int fd = ConnectionNumber(s->dpy);
    for (;;) {
        XFlush(s->dpy);
        while (XPending(s->dpy)) {
            XEvent e;
            XNextEvent(s->dpy, &e);
            int r = x11_translate(s, &e);
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

void *vs_native_display(void *vs) { return ((VSurf *)vs)->dpy; }
unsigned long vs_native_window(void *vs) { return (unsigned long)((VSurf *)vs)->win; }
void *vs_native_gc(void *vs) { return ((VSurf *)vs)->gc; }

#endif /* _WIN32 / X11 */
