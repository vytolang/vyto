/* vyto/hw/input native backing — input devices via Linux evdev.

   The Vyto-way streaming hardware pattern (docs/HARDWARE.md): an input device is a
   fd under /dev/input/event*, opened non-blocking so it folds into a PollSet event
   loop alongside sockets, windows, and serial ports — one wait() covers the network,
   the wire, AND the gamepad/keyboard/touch. Two halves live here:

     - Enumeration (the usb half): input_enumerate() snapshots every /dev/input/event*
       into an opaque InputList, reading each device's name/id, then closing it. The
       Vyto side copies the fields out and frees the handle.
     - Streaming (the serial half): evdev_open() hands back a non-blocking fd;
       evdev_next() reads exactly one input_event into a file-static struct and the
       accessors read its type/code/value out. Parsing the event in C keeps the
       boundary at plain ints — struct input_event's timeval layout is arch-dependent,
       so it never crosses as a raw byte[]. Single-threaded (Vyto has no threads), so
       the static is safe, same reasoning as the usb accessors.

   Zero #link, read-only enumeration; opening event nodes usually needs the `input`
   group (soft: absent/denied -> empty/-1).

   Windows: Raw Input, for keyboards and mice (below). Other platforms compile to
   empty/-1 stubs. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    char path[512];    /* /dev/input/eventN, or a Windows interface path */
    char name[128];
    int  vendor, product;
} InputInfo;

typedef struct {
    InputInfo *v;
    int n, cap;
} InputList;

#if defined(__linux__) && !defined(VT_NO_LIBC)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static void list_push(InputList *l, const InputInfo *d) {
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 16;
        InputInfo *nv = (InputInfo *)realloc(l->v, (size_t)want * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = want;
    }
    l->v[l->n++] = *d;
}

InputList *input_enumerate(void) {
    InputList *l = (InputList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    const char *root = "/dev/input";
    DIR *dp = opendir(root);
    if (!dp) return l; /* no input subsystem — empty list, not an error */
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        InputInfo d;
        memset(&d, 0, sizeof d);
        snprintf(d.path, sizeof d.path, "%s/%s", root, de->d_name);
        int fd = open(d.path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue; /* no permission on this node — skip it */
        char nm[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof nm - 1), nm) >= 0) {
            snprintf(d.name, sizeof d.name, "%s", nm);
        }
        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) >= 0) {
            d.vendor = id.vendor;
            d.product = id.product;
        }
        close(fd);
        list_push(l, &d);
    }
    closedir(dp);
    return l;
}

/* Open an event node non-blocking. Returns the fd, or -1 (missing / no permission). */
int evdev_open(const char *path) {
    return open(path, O_RDONLY | O_NONBLOCK);
}

/* The last event read by evdev_next (single-threaded, so a file-static is safe). */
static struct input_event g_ev;

/* Read exactly one input_event. 1 = got one (accessors valid), 0 = would-block / EOF,
   -1 = error. A partial read (never expected on a chardev) counts as would-block. */
int evdev_next(int fd) {
    ssize_t r = read(fd, &g_ev, sizeof g_ev);
    if (r == (ssize_t)sizeof g_ev) return 1;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (r == 0) return 0; /* EOF (device unplugged) */
    return (r < 0) ? -1 : 0;
}

int evdev_type(void)  { return (int)g_ev.type; }
int evdev_code(void)  { return (int)g_ev.code; }
int evdev_value(void) { return (int)g_ev.value; }

void evdev_close(int fd) { if (fd >= 0) close(fd); }
int evdev_poll_fd(int fd) { return fd; }

#elif defined(_WIN32) && !defined(VT_NO_LIBC)
/* Windows: Raw Input, keyboards and mice.

   input_devices() is GetRawInputDeviceList: every keyboard and mouse Windows
   knows, by the interface path it reports, which is what input_open takes.
   Events come as Linux evdev numbers, so one program reads both platforms:
   keys as EV_KEY with Linux KEY_* codes (PC scan code set 1 is exactly Linux's
   numbering for the plain keys; the E0-prefixed ones are mapped by e0_key)
   and value 1 press / 0 release / 2 autorepeat; mice as EV_KEY BTN_*, EV_REL
   REL_X/REL_Y/REL_WHEEL/REL_HWHEEL (EV_ABS ABS_X/ABS_Y for absolute devices
   such as tablets and remote sessions); each packet ends in EV_SYN.

   Raw Input is registered per process and per usage: a second window asking
   for keyboard input would silently take it from the first. So there is one
   message-only window for the whole process, on one thread, registered for
   keyboards and mice with RIDEV_INPUTSINK (input arrives whether or not this
   program is in the foreground, which is the point for a barcode scanner
   working beside another app) and RIDEV_DEVNOTIFY (it hears removals). It
   routes each WM_INPUT by device handle to the open devices, each of which has
   its own loopback socket pair; the Vyto side polls its end. Events are 12
   bytes each (type, code, value as little-endian int32). A device nobody reads
   does not stall the others: when its socket buffer is full, its new events
   are dropped. A removed device's stream is closed, which Vyto sees as the
   end. Input aimed at an elevated window is not delivered (UIPI). */
#include "vyto_win_pump.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include "vyto_win.h"

static void list_push(InputList *l, const InputInfo *d) {
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 16;
        InputInfo *nv = (InputInfo *)realloc(l->v, (size_t)want * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = want;
    }
    l->v[l->n++] = *d;
}

static int hex4_after(const char *s, const char *tag) {
    const char *p = strstr(s, tag);
    if (!p) return 0;
    char b[5];
    memcpy(b, p + strlen(tag), 4);
    b[4] = 0;
    return (int)strtol(b, NULL, 16);
}

static const DEVPROPKEY in_PKEY_BusReportedDeviceDesc =
    {{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};

/* The product name for an interface path: the USB device's own string when
   it is on USB (the interface's node or one of its parents, through a HID
   collection), else the driver's description. */
static void name_of(const WCHAR *path, char *out, size_t cap) {
    out[0] = 0;
    HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA id;
    memset(&id, 0, sizeof id);
    id.cbSize = sizeof id;
    if (SetupDiOpenDeviceInterfaceW(set, path, 0, &id)) {
        SP_DEVINFO_DATA dd;
        memset(&dd, 0, sizeof dd);
        dd.cbSize = sizeof dd;
        if (SetupDiGetDeviceInterfaceDetailW(set, &id, NULL, 0, NULL, &dd) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            WCHAR w[256];
            DEVPROPTYPE pt = 0;
            DEVINST cur = dd.DevInst;
            for (int up = 0; up < 3; up++) {
                ULONG sz = sizeof w - 2;
                if (CM_Get_DevNode_PropertyW(cur, &in_PKEY_BusReportedDeviceDesc, &pt, (BYTE *)w,
                                             &sz, 0) == CR_SUCCESS && pt == DEVPROP_TYPE_STRING) {
                    w[255] = 0;
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
                    out[cap - 1] = 0;
                    break;
                }
                DEVINST parent;
                if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
                cur = parent;
            }
            if (!out[0] && SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_DEVICEDESC, NULL,
                                                             (BYTE *)w, sizeof w - 2, NULL)) {
                w[255] = 0;
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
                out[cap - 1] = 0;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

static int raw_name(HANDLE h, WCHAR *w, UINT cap) {
    UINT n = cap;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, w, &n) == (UINT)-1) return 0;
    w[cap - 1] = 0;
    /* XP reported "\??\" where everything later says "\\?\" */
    if (w[0] == L'\\' && w[1] == L'?' && w[2] == L'?' && w[3] == L'\\') w[1] = L'\\';
    return 1;
}

InputList *input_enumerate(void) {
    InputList *l = (InputList *)calloc(1, sizeof *l);
    if (!l) return NULL;
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return l;
    RAWINPUTDEVICELIST *v = (RAWINPUTDEVICELIST *)calloc(n, sizeof *v);
    if (!v) return l;
    UINT got = GetRawInputDeviceList(v, &n, sizeof *v);
    for (UINT i = 0; got != (UINT)-1 && i < got; i++) {
        if (v[i].dwType != RIM_TYPEKEYBOARD && v[i].dwType != RIM_TYPEMOUSE) continue;
        WCHAR w[512];
        if (!raw_name(v[i].hDevice, w, 512)) continue;
        InputInfo d;
        memset(&d, 0, sizeof d);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, d.path, (int)sizeof d.path, NULL, NULL);
        d.path[sizeof d.path - 1] = 0;
        char low[sizeof d.path];
        size_t k = 0;
        for (; d.path[k] && k < sizeof low - 1; k++)
            low[k] = (char)(d.path[k] >= 'A' && d.path[k] <= 'Z' ? d.path[k] + 32 : d.path[k]);
        low[k] = 0;
        d.vendor = hex4_after(low, "vid_");
        d.product = hex4_after(low, "pid_");
        name_of(w, d.name, sizeof d.name);
        if (!d.name[0])
            snprintf(d.name, sizeof d.name, "%s", v[i].dwType == RIM_TYPEKEYBOARD ? "Keyboard" : "Mouse");
        list_push(l, &d);
    }
    free(v);
    return l;
}

/* ---- the process-wide raw input window ---------------------------------- */

#define IN_SLOTS 32

typedef struct {
    HANDLE dev;        /* the raw input device handle */
    SOCKET vy, th;     /* Vyto's end (polled), the router's end */
    int closed;        /* the router ended this stream (device removed) */
    unsigned char down[512];  /* keys held, for autorepeat (value 2) */
} InSlot;

static InSlot *g_in[IN_SLOTS];
static CRITICAL_SECTION g_in_lock;
static HANDLE g_in_ready;
static volatile LONG g_in_state;    /* 0 not started, 1 running, -1 failed */

/* Linux key codes for the E0-prefixed set-1 scan codes. */
static int e0_key(int sc) {
    switch (sc) {
    case 0x1C: return 96;  case 0x1D: return 97;  case 0x35: return 98;  case 0x37: return 99;
    case 0x38: return 100; case 0x47: return 102; case 0x48: return 103; case 0x49: return 104;
    case 0x4B: return 105; case 0x4D: return 106; case 0x4F: return 107; case 0x50: return 108;
    case 0x51: return 109; case 0x52: return 110; case 0x53: return 111; case 0x5B: return 125;
    case 0x5C: return 126; case 0x5D: return 127; case 0x5E: return 116; case 0x5F: return 142;
    case 0x10: return 165; case 0x19: return 163; case 0x20: return 113; case 0x22: return 164;
    case 0x24: return 166; case 0x2E: return 114; case 0x30: return 115;
    default:   return 0;
    }
}

static void put_ev(char *b, int *n, int type, int code, int value) {
    int32_t v[3] = { type, code, value };
    memcpy(b + *n, v, 12);
    *n += 12;
}

static void on_input(HRAWINPUT hri) {
    union { RAWINPUT ri; char pad[256]; } u;
    UINT sz = sizeof u;
    if (GetRawInputData(hri, RID_INPUT, &u, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    RAWINPUT *ri = &u.ri;
    char b[12 * 12];
    EnterCriticalSection(&g_in_lock);
    for (int i = 0; i < IN_SLOTS; i++) {
        InSlot *s = g_in[i];
        if (!s || s->closed || s->dev != ri->header.hDevice) continue;
        int n = 0;
        if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD *k = &ri->data.keyboard;
            int sc = k->MakeCode;
            if (sc == 0 || sc > 0xFF) continue;   /* fake keys (Pause's second half) */
            int code = (k->Flags & RI_KEY_E0) ? e0_key(sc) : sc;
            if (code <= 0 || code >= 512) continue;
            int up = (k->Flags & RI_KEY_BREAK) != 0;
            int value = up ? 0 : (s->down[code] ? 2 : 1);
            s->down[code] = (unsigned char)!up;
            put_ev(b, &n, 0x01, code, value);
        } else if (ri->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE *m = &ri->data.mouse;
            USHORT f = m->usButtonFlags;
            static const struct { USHORT dn, upf; int btn; } bt[] = {
                {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0x110},
                {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 0x111},
                {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 0x112},
                {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 0x113},
                {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 0x114},
            };
            for (size_t j = 0; j < sizeof bt / sizeof bt[0]; j++) {
                if (f & bt[j].dn) put_ev(b, &n, 0x01, bt[j].btn, 1);
                if (f & bt[j].upf) put_ev(b, &n, 0x01, bt[j].btn, 0);
            }
            if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
                put_ev(b, &n, 0x03, 0x00, (int)m->lLastX);
                put_ev(b, &n, 0x03, 0x01, (int)m->lLastY);
            } else {
                if (m->lLastX) put_ev(b, &n, 0x02, 0x00, (int)m->lLastX);
                if (m->lLastY) put_ev(b, &n, 0x02, 0x01, (int)m->lLastY);
            }
            /* One notch is 120; Linux counts notches. */
            if (f & RI_MOUSE_WHEEL) put_ev(b, &n, 0x02, 0x08, (int)(SHORT)m->usButtonData / 120);
            if (f & 0x0800 /* RI_MOUSE_HWHEEL */)
                put_ev(b, &n, 0x02, 0x06, (int)(SHORT)m->usButtonData / 120);
        }
        if (n > 0) {
            put_ev(b, &n, 0x00, 0, 0);   /* EV_SYN SYN_REPORT */
            send(s->th, b, n, 0);        /* non-blocking: a full buffer drops it */
        }
    }
    LeaveCriticalSection(&g_in_lock);
}

static LRESULT CALLBACK in_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        on_input((HRAWINPUT)lp);
        return DefWindowProcW(hwnd, msg, wp, lp);   /* lets Windows free the input */
    }
    if (msg == 0x00FE /* WM_INPUT_DEVICE_CHANGE */ && wp == 2 /* GIDC_REMOVAL */) {
        EnterCriticalSection(&g_in_lock);
        for (int i = 0; i < IN_SLOTS; i++) {
            InSlot *s = g_in[i];
            if (s && s->dev == (HANDLE)lp && !s->closed) {
                shutdown(s->th, SD_SEND);
                s->closed = 1;
            }
        }
        LeaveCriticalSection(&g_in_lock);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI in_thread(LPVOID arg) {
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = in_wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"VytoRawInput";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"VytoRawInput", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    RAWINPUTDEVICE r[2];
    memset(r, 0, sizeof r);
    r[0].usUsagePage = 1; r[0].usUsage = 6;            /* keyboard */
    r[1].usUsagePage = 1; r[1].usUsage = 2;            /* mouse */
    for (int i = 0; i < 2; i++) {
        r[i].dwFlags = RIDEV_INPUTSINK | 0x00002000 /* RIDEV_DEVNOTIFY */;
        r[i].hwndTarget = hwnd;
    }
    int ok = hwnd && RegisterRawInputDevices(r, 2, sizeof r[0]);
    InterlockedExchange(&g_in_state, ok ? 1 : -1);
    SetEvent(g_in_ready);
    if (!ok) return 0;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

/* Start the router once per process. 1 when it is running. */
static int in_router(void) {
    static LONG once;
    if (!InterlockedExchange(&once, 1)) {
        InitializeCriticalSection(&g_in_lock);
        g_in_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE t = g_in_ready ? CreateThread(NULL, 64 * 1024, in_thread, NULL,
                                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
        if (!t) InterlockedExchange(&g_in_state, -1);
        else {
            WaitForSingleObject(g_in_ready, 5000);
            CloseHandle(t);
        }
    }
    return g_in_state == 1;
}

/* Open a keyboard or mouse by the path input_devices() gave. A slot, or -1
   (no such device, or the router could not start). */
int evdev_open(const char *path) {
    if (!path || !*path) return -1;
    VtWBuf want;
    vt_wbuf_from(&want, path);
    HANDLE dev = NULL;
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) == 0 && n > 0) {
        RAWINPUTDEVICELIST *v = (RAWINPUTDEVICELIST *)calloc(n, sizeof *v);
        UINT got = v ? GetRawInputDeviceList(v, &n, sizeof *v) : (UINT)-1;
        for (UINT i = 0; got != (UINT)-1 && i < got && !dev; i++) {
            WCHAR w[512];
            if ((v[i].dwType == RIM_TYPEKEYBOARD || v[i].dwType == RIM_TYPEMOUSE) &&
                raw_name(v[i].hDevice, w, 512) && _wcsicmp(w, want.p) == 0)
                dev = v[i].hDevice;
        }
        free(v);
    }
    vt_wbuf_free(&want);
    if (!dev || !in_router()) return -1;
    InSlot *s = (InSlot *)calloc(1, sizeof *s);
    if (!s) return -1;
    if (!vt_pump_pair(&s->vy, &s->th)) { free(s); return -1; }
    u_long nb = 1;
    ioctlsocket(s->vy, FIONBIO, &nb);
    ioctlsocket(s->th, FIONBIO, &nb);   /* a full buffer drops, never blocks the router */
    s->dev = dev;
    int fd = -1;
    EnterCriticalSection(&g_in_lock);
    for (int i = 0; i < IN_SLOTS; i++) if (!g_in[i]) { g_in[i] = s; fd = i; break; }
    LeaveCriticalSection(&g_in_lock);
    if (fd < 0) { closesocket(s->vy); closesocket(s->th); free(s); }
    return fd;
}

int evdev_poll_fd(int fd) {
    InSlot *s = (fd >= 0 && fd < IN_SLOTS) ? g_in[fd] : NULL;
    return s ? (int)s->vy : -1;
}

static int32_t g_ev[3];

/* One event: 1 got, 0 none yet, -1 the device is gone. Takes a record only
   once all 12 bytes are there, so a split read never misaligns the stream. */
int evdev_next(int fd) {
    InSlot *s = (fd >= 0 && fd < IN_SLOTS) ? g_in[fd] : NULL;
    if (!s) return -1;
    char b[12];
    int n = recv(s->vy, b, 12, MSG_PEEK);
    if (n == 0) return -1;
    if (n < 0) return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
    if (n < 12) return 0;
    recv(s->vy, b, 12, 0);
    memcpy(g_ev, b, 12);
    return 1;
}

int evdev_type(void)  { return (int)g_ev[0]; }
int evdev_code(void)  { return (int)g_ev[1]; }
int evdev_value(void) { return (int)g_ev[2]; }

void evdev_close(int fd) {
    InSlot *s = (fd >= 0 && fd < IN_SLOTS) ? g_in[fd] : NULL;
    if (!s) return;
    EnterCriticalSection(&g_in_lock);
    g_in[fd] = NULL;
    LeaveCriticalSection(&g_in_lock);
    closesocket(s->vy);
    closesocket(s->th);
    free(s);
}

#else /* other platforms: empty list, no device access */

InputList *input_enumerate(void) { return (InputList *)calloc(1, sizeof(InputList)); }
int  evdev_open(const char *path) { (void)path; return -1; }
int  evdev_next(int fd) { (void)fd; return -1; }
int  evdev_type(void)  { return 0; }
int  evdev_code(void)  { return 0; }
int  evdev_value(void) { return 0; }
void evdev_close(int fd) { (void)fd; }
int  evdev_poll_fd(int fd) { (void)fd; return -1; }

#endif

int input_count(InputList *l) { return l ? l->n : 0; }

static InputInfo *at(InputList *l, int i) {
    if (!l || i < 0 || i >= l->n) return NULL;
    return &l->v[i];
}
const char *input_path(InputList *l, int i)   { InputInfo *d = at(l, i); return d ? d->path : ""; }
const char *input_name(InputList *l, int i)   { InputInfo *d = at(l, i); return d ? d->name : ""; }
int         input_vendor(InputList *l, int i)  { InputInfo *d = at(l, i); return d ? d->vendor : 0; }
int         input_product(InputList *l, int i) { InputInfo *d = at(l, i); return d ? d->product : 0; }

void input_free(InputList *l) {
    if (!l) return;
    free(l->v);
    free(l);
}
