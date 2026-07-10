/* vsurf backends:
 *   - Win32 GDI when compiling for Windows (_WIN32)
 *   - X11 everywhere else
 *   - headless (VS_HEADLESS=1): no window system at all; events replay from
 *     the $VS_EVENTS script. Shared by every platform, used by the tests.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */

#include "vsurf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int headless_on(void);
static long long headless_ms; /* synthetic clock: +16 per scripted event */

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

/* $VOLT_SCALE (e.g. "1.5" or "150") overrides platform DPI detection */
static int scale_from_env(void) {
    const char *e = getenv("VOLT_SCALE");
    if (!e || !*e) return 0;
    double f = atof(e);
    if (f > 8.0) return (int)(f + 0.5); /* given as percent already */
    if (f >= 0.5) return (int)(f * 100.0 + 0.5);
    return 0;
}

static int last_key, last_x, last_y, last_wheel, last_mods;
static char last_text[32];

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
 *   rclick X Y       right-button mouse down at X,Y (VS_EV_MOUSE_RDOWN, no
 *                    paired up event — matches the real backends, which
 *                    don't deliver a right-button release)
 *   resize W H
 *   expose
 *   tick             a timer tick (VS_EV_TIMER), for game-loop tests
 *   mods <spec>      set the modifier state reported by vs_mods() for all
 *                    following events: "+"-joined ctrl/shift/alt/super, or
 *                    "none" (e.g. "mods ctrl+shift"). Emits no event itself.
 *   close
 * '#' starts a comment; EOF acts as close. */
static int headless_wait(int *w, int *h) {
    static int pending_up; /* deliver MOUSE_UP after each click's MOUSE_DOWN */
    headless_ms += 16;     /* one synthetic frame per delivered event */
    if (pending_up) {
        pending_up = 0;
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
            last_key = len == 1 ? (int)lead : 0;
            int i = 0;
            while (i < len && type_p[i]) { last_text[i] = type_p[i]; i++; }
            last_text[i] = 0;
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
            last_text[0] = 0;
            int code = key_by_name(k);
            if (!code) continue;
            last_key = code;
            if (code == VS_KEY_SPACE) { last_text[0] = ' '; last_text[1] = 0; }
            return up ? VS_EV_KEY_UP : VS_EV_KEY;
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
            last_mods = m;
            continue;
        }
        if (sscanf(script_line, "click %d %d", &last_x, &last_y) == 2) {
            pending_up = 1;
            return VS_EV_MOUSE_DOWN;
        }
        if (sscanf(script_line, "rclick %d %d", &last_x, &last_y) == 2) {
            return VS_EV_MOUSE_RDOWN;
        }
        if (sscanf(script_line, "down %d %d", &last_x, &last_y) == 2) {
            return VS_EV_MOUSE_DOWN;
        }
        if (sscanf(script_line, "up %d %d", &last_x, &last_y) == 2) {
            return VS_EV_MOUSE_UP;
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
            int mx, my;
            if (sscanf(script_line, "move %d %d", &mx, &my) == 2) {
                last_x = mx;
                last_y = my;
                return VS_EV_MOUSE_MOVE;
            }
        }
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
int vs_mods(void) { return last_mods; }

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
    int type, key, x, y, mods;
    char text[2];
} QEv;
static QEv evq[64];
static int evq_head, evq_len;

/* async modifier state at event time */
static int win_mods(void) {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= VS_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= VS_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= VS_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= VS_MOD_SUPER;
    return m;
}

static void q_push(int type, int key, int x, int y, int ch) {
    /* coalesce mouse-move bursts: overwrite a trailing queued move */
    if (type == VS_EV_MOUSE_MOVE && evq_len > 0) {
        QEv *tail = &evq[(evq_head + evq_len - 1) % 64];
        if (tail->type == VS_EV_MOUSE_MOVE) {
            tail->x = x;
            tail->y = y;
            tail->mods = win_mods();
            return;
        }
    }
    if (evq_len == 64) {
        /* drop when full — except CLOSE, which must never be lost */
        if (type != VS_EV_CLOSE) return;
        evq_len--; /* sacrifice the newest queued event for the CLOSE */
    }
    QEv *e = &evq[(evq_head + evq_len) % 64];
    e->type = type;
    e->key = key;
    e->x = x;
    e->y = y;
    e->mods = win_mods();
    e->text[0] = (char)ch;
    e->text[1] = 0;
    evq_len++;
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
    case WM_RBUTTONDOWN:
        q_push(VS_EV_MOUSE_RDOWN, 0, (short)LOWORD(lp), (short)HIWORD(lp), 0);
        return 0;
    case WM_MOUSEMOVE:
        q_push(VS_EV_MOUSE_MOVE, 0, (short)LOWORD(lp), (short)HIWORD(lp), 0);
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
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: { /* SYS variant: Alt itself and Alt-held chords */
        int k = vk_to_key(wp);
        if (!k && (GetKeyState(VK_CONTROL) & 0x8000)) {
            /* Ctrl chords produce control-char WM_CHARs; report the plain
               key here instead so shortcuts see 'a', not 0x01 (no text) */
            if (wp >= 'A' && wp <= 'Z') k = (int)wp + 32;
            else if (wp >= '0' && wp <= '9') k = (int)wp;
            else if (wp == VK_SPACE) k = VS_KEY_SPACE;
        }
        if (k) q_push(VS_EV_KEY, k, 0, 0, 0);
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
        if (k) q_push(VS_EV_KEY_UP, k, 0, 0, 0);
        if (msg == WM_KEYUP) return 0;
        break;
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
        g_scale_pct = 100;
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

void vs_blit(void *vs, const int *pixels, int srcw, int srch,
             int dstx, int dsty, int dstw, int dsth) {
    VSurf *s = vs;
    if (!s->hwnd || srcw <= 0 || srch <= 0) return;
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
    /* repack the sub-rect tightly; SetDIBitsToDevice's source-offset rules
       for top-down DIBs are error-prone, a copy is simple and correct */
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

int vs_scale_pct(void) {
    int env = scale_from_env();
    if (env) return env;
    return g_scale_pct ? g_scale_pct : 100;
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
    last_mods = e.mods;
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
        /* MWMO_INPUTAVAILABLE: also wake for input already in the queue that
           a previous PeekMessage saw — plain MsgWaitForMultipleObjects only
           signals NEW input and can stall on already-queued messages */
        DWORD r = MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)ms, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
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
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <locale.h>
#include <sys/select.h>

typedef struct VSurf {
    Display *dpy; /* NULL in headless mode */
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
static int x11_detect_scale(Display *dpy) {
    const char *rms = XResourceManagerString(dpy);
    if (rms) {
        const char *p = strstr(rms, "Xft.dpi:");
        if (p) {
            double dpi = atof(p + 8);
            if (dpi >= 48.0) return (int)(dpi * 100.0 / 96.0 + 0.5);
        }
    }
    int scr = DefaultScreen(dpy);
    int mm = DisplayWidthMM(dpy, scr);
    if (mm > 0) {
        double dpi = (double)DisplayWidth(dpy, scr) * 25.4 / (double)mm;
        if (dpi >= 48.0 && dpi <= 480.0) return (int)(dpi * 100.0 / 96.0 + 0.5);
    }
    return 100;
}

void *vs_open(const char *title, int w, int h) {
    VSurf *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    if (headless_on()) {
        headless_open_script();
        g_scale_pct = 100;
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
    s->win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)w, (unsigned)h, 1,
                                 BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, s->win, title);
    XSelectInput(dpy, s->win, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                                  ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    /* only emit KeyRelease on real release, not auto-repeat — matters for
       held-key game input (EV_KEY / EV_KEY_UP pairing) */
    XkbSetDetectableAutoRepeat(dpy, True, NULL);
    s->wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, s->win, &s->wm_delete, 1);
    s->gc = XCreateGC(dpy, s->win, 0, 0);
    s->pgc = XCreateGC(dpy, s->win, 0, 0);
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

void vs_close(void *vs) {
    VSurf *s = vs;
    if (!s) return;
    if (s->dpy) {
        if (s->img) { XDestroyImage(s->img); s->img = NULL; } /* frees its data too */
        if (s->ic) XDestroyIC(s->ic);
        if (s->im) XCloseIM(s->im);
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
    if (!s->dpy) return;
    XSetClipMask(s->dpy, s->gc, None);
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
    if (!s->dpy || srcw <= 0 || srch <= 0 || dstw <= 0 || dsth <= 0) return;
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
    if (!s->dpy || w <= 0 || h <= 0 || stride_px <= 0) return;
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
    if (!s->dpy) return;
    XCopyArea(s->dpy, s->back, s->win, s->pgc, 0, 0, (unsigned)s->w, (unsigned)s->h, 0, 0);
    XFlush(s->dpy);
}

void vs_present_rect(void *vs, int x, int y, int w, int h) {
    VSurf *s = vs;
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

/* decode an X key event into last_key/last_text; 1 if a key we deliver, else 0 */
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
    last_text[0] = 0;
    switch (ks) {
    case XK_Return: case XK_KP_Enter: last_key = VS_KEY_ENTER; return 1;
    case XK_BackSpace: last_key = VS_KEY_BACKSPACE; return 1;
    case XK_Escape: last_key = VS_KEY_ESC; return 1;
    case XK_Up: case XK_KP_Up: last_key = VS_KEY_UP; return 1;
    case XK_Down: case XK_KP_Down: last_key = VS_KEY_DOWN; return 1;
    case XK_Left: case XK_KP_Left: last_key = VS_KEY_LEFT; return 1;
    case XK_Right: case XK_KP_Right: last_key = VS_KEY_RIGHT; return 1;
    case XK_Delete: case XK_KP_Delete: last_key = VS_KEY_DELETE; return 1;
    case XK_Tab: case XK_ISO_Left_Tab: last_key = VS_KEY_TAB; return 1;
    case XK_Home: case XK_KP_Home: last_key = VS_KEY_HOME; return 1;
    case XK_End: case XK_KP_End: last_key = VS_KEY_END; return 1;
    case XK_Page_Up: case XK_KP_Page_Up: last_key = VS_KEY_PAGEUP; return 1;
    case XK_Page_Down: case XK_KP_Page_Down: last_key = VS_KEY_PAGEDOWN; return 1;
    case XK_Insert: case XK_KP_Insert: last_key = VS_KEY_INSERT; return 1;
    case XK_Shift_L: case XK_Shift_R: last_key = VS_KEY_SHIFT; return 1;
    case XK_Control_L: case XK_Control_R: last_key = VS_KEY_CTRL; return 1;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R:
        last_key = VS_KEY_ALT; return 1;
    case XK_Super_L: case XK_Super_R: last_key = VS_KEY_SUPER; return 1;
    default:
        if (ks >= XK_F1 && ks <= XK_F12) { /* contiguous keysym block */
            last_key = VS_KEY_F1 + (int)(ks - XK_F1);
            return 1;
        }
        /* Printable key: prefer the (shift-aware) keysym so Ctrl+A still
           reads as 'a' — the lookup folds Ctrl into a control byte, which
           would otherwise swallow the shortcut key entirely. Text is set
           only from a printable lookup, so Ctrl-chorded keys deliver a key
           code but no insertable text. */
        if (ks == XK_space || (ks >= 0x21 && ks <= 0x7e)) {
            last_key = (int)ks;
            if (n >= 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
                if ((size_t)n < sizeof last_text) memcpy(last_text, buf, (size_t)n + 1);
            }
            return 1;
        }
        /* keypad digits, and any UTF-8 text the input method composed:
           no ASCII key code (key = 0 for multibyte), only insertable text */
        if (n >= 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
            last_key = (n == 1 && (unsigned char)buf[0] < 127) ? buf[0] : 0;
            if ((size_t)n < sizeof last_text) memcpy(last_text, buf, (size_t)n + 1);
            return 1;
        }
    }
    return 0; /* dead key or unmapped */
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
        last_mods = x11_mods_of(e->xkey.state);
        return x11_decode_key(s, &e->xkey) ? VS_EV_KEY : -1;
    case KeyRelease:
        last_mods = x11_mods_of(e->xkey.state);
        return x11_decode_key(s, &e->xkey) ? VS_EV_KEY_UP : -1;
    case ButtonPress:
        last_mods = x11_mods_of(e->xbutton.state);
        if (e->xbutton.button == Button1) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            return VS_EV_MOUSE_DOWN;
        }
        if (e->xbutton.button == Button3) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            return VS_EV_MOUSE_RDOWN;
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
        last_mods = x11_mods_of(e->xbutton.state);
        if (e->xbutton.button == Button1) {
            last_x = e->xbutton.x;
            last_y = e->xbutton.y;
            return VS_EV_MOUSE_UP;
        }
        return -1;
    case MotionNotify:
        last_mods = x11_mods_of(e->xmotion.state);
        /* hover source — coalesce: only deliver if the cursor actually moved.
           X11 often fires many MotionNotify per pixel; throttle by storing the
           latest and letting vs_wait dedupe via the change check in Window. */
        last_x = e->xmotion.x;
        last_y = e->xmotion.y;
        return VS_EV_MOUSE_MOVE;
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == s->wm_delete) return VS_EV_CLOSE;
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
        last_x = e.xmotion.x;
        last_y = e.xmotion.y;
        last_mods = x11_mods_of(e.xmotion.state);
    }
}

/* translate + IME filter + motion coalescing for one raw event */
static int x11_deliver(VSurf *s, XEvent *e) {
    if (XFilterEvent(e, None)) return -1; /* consumed by the input method */
    int r = x11_translate(s, e);
    if (r == VS_EV_MOUSE_MOVE) x11_coalesce_motion(s);
    return r;
}

int vs_wait(void *vs) {
    VSurf *s = vs;
    if (!s->dpy) return headless_wait(&s->w, &s->h);
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
    if (!s->dpy) return headless_wait(&s->w, &s->h);
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
    if (!s->dpy) return headless_wait(&s->w, &s->h);
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

int vs_scale_pct(void) {
    int env = scale_from_env();
    if (env) return env;
    return g_scale_pct ? g_scale_pct : 100;
}

void *vs_native_display(void *vs) { return ((VSurf *)vs)->dpy; }
unsigned long vs_native_window(void *vs) { return (unsigned long)((VSurf *)vs)->win; }
void *vs_native_gc(void *vs) { return ((VSurf *)vs)->gc; }

#endif /* _WIN32 / X11 */
