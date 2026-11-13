/* vyto/hw/uevent native backing — kernel device events via NETLINK_KOBJECT_UEVENT.

   The push-event source the sysfs packages (hw/power, net/link) lack: the kernel
   broadcasts a uevent whenever a device is added/removed/changed — AC unplugged,
   battery capacity crossed a threshold, cable pulled, USB device attached. It arrives
   on a single poll-able netlink fd, so it folds into a PollSet next to sockets, camera,
   and serial — one wait() covers every hardware change in the system.

   Each message is a datagram of NUL-separated fields: a "ACTION@DEVPATH" header then
   KEY=VALUE lines (SUBSYSTEM, DEVPATH, ACTION, and subsystem-specific keys like
   POWER_SUPPLY_ONLINE / INTERFACE). The shim turns the NULs into newlines so the Vyto
   side can split on "\n" (netlink messages carry no embedded NUL that matters here).

   Receiving kernel uevents on multicast group 1 is normally unprivileged.

   Windows has no such stream. The nearest is what the system tells a window:
   device-interface arrival and removal (RegisterDeviceNotification, every
   interface class) and power-setting changes (RegisterPowerSettingNotification:
   AC/battery, battery level). A pump thread (runtime/vyto_win_pump.h) owns a
   message-only window that receives them and writes each one as a Linux-shaped
   event, so the Vyto side parses both the same way. Other platforms compile to
   -1 stubs. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(VT_NO_LIBC)
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>

/* Open a netlink uevent socket subscribed to the kernel's broadcast group, non-blocking.
   Returns the fd, or -1. */
int uevent_open(void) {
    int fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = 0;          /* let the kernel assign a unique pid */
    sa.nl_groups = 1;       /* group 1 = kernel-generated uevents */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

/* Read one uevent datagram, NULs rewritten to '\n', NUL-terminated. >0 bytes, -1
   would-block (nothing pending), -2 error. Read after the fd polls readable. */
int64_t uevent_read(int fd, char *buf, int64_t cap) {
    long n = (long)recv(fd, buf, (size_t)cap - 1, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    for (long i = 0; i < n; i++) if (buf[i] == 0) buf[i] = '\n';
    buf[n] = 0;
    return n;
}

void uevent_close(int fd) { if (fd >= 0) close(fd); }
int uevent_poll_fd(int fd) { return fd; }

#elif defined(_WIN32) && !defined(VT_NO_LIBC)
/* Each event the window receives becomes text in the pump's stream:
     "add@\\?\USB#VID_0403&PID_6001#A50285BI#{a5dcbf10-...}\n"
     "ACTION=add\nDEVPATH=...\nSUBSYSTEM=usb\nDEVICE_CLASS={...}\nPRODUCT=403/6001/0\n"
   ending in one NUL, which is the frame boundary: a stream socket has none of
   its own, so uevent_read peeks for the NUL and takes exactly one event.

   SUBSYSTEM is the Linux name for the interface class where there is one
   (usb, tty, hidraw, input, block, net), else "unknown". Power changes are
   "change@/power_supply/AC" (POWER_SUPPLY_ONLINE) and ".../BAT0"
   (POWER_SUPPLY_CAPACITY), as vyto/hw/power names them. */
#include "vyto_win_pump.h"
#include <dbt.h>
#include <stdio.h>

static const GUID ue_GUID_ACDC = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
static const GUID ue_GUID_BATPCT = {0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}};

static const struct { GUID g; const char *sub; } ue_classes[] = {
    {{0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}}, "usb"},
    {{0x86e0d1e0, 0x8089, 0x11d0, {0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73}}, "tty"},
    {{0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}}, "hidraw"},
    {{0x884b96c3, 0x56ef, 0x11d1, {0xbc, 0x8c, 0x00, 0xa0, 0xc9, 0x14, 0x05, 0xdd}}, "input"},
    {{0x378de44c, 0x56ef, 0x11d1, {0xbc, 0x8c, 0x00, 0xa0, 0xc9, 0x14, 0x05, 0xdd}}, "input"},
    {{0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}}, "block"},
    {{0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}}, "block"},
    {{0xcac88484, 0x7515, 0x4c03, {0x82, 0xe6, 0x71, 0xa8, 0x7a, 0xba, 0xc3, 0x61}}, "net"},
};

#define UE_SLOTS 16
#define UE_QCAP 16384

typedef struct {
    VtPump *pump;
    HWND hwnd;
    HDEVNOTIFY dev;
    HPOWERNOTIFY pw[2];
    int seen[2];            /* the first power callback is the current value, not a change */
    char q[UE_QCAP];        /* events the window queued, not yet handed to the pump */
    int qlen;
} UeMon;

static UeMon *g_ue[UE_SLOTS];

static void ue_push(UeMon *m, const char *text) {
    int n = (int)strlen(text) + 1;           /* with its NUL */
    if (m->qlen + n > UE_QCAP) return;       /* a burst nobody reads: drop, do not grow */
    memcpy(m->q + m->qlen, text, (size_t)n);
    m->qlen += n;
}

static void ue_guid_text(const GUID *g, char *out, size_t cap) {
    snprintf(out, cap, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
             g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static void ue_device(UeMon *m, const char *action, const DEV_BROADCAST_DEVICEINTERFACE_W *d) {
    char path[600], cls[48], ev[1600];
    int k = WideCharToMultiByte(CP_UTF8, 0, d->dbcc_name, -1, path, (int)sizeof path, NULL, NULL);
    if (k <= 0) path[0] = 0;
    path[sizeof path - 1] = 0;
    const char *sub = "unknown";
    for (size_t i = 0; i < sizeof ue_classes / sizeof ue_classes[0]; i++)
        if (memcmp(&ue_classes[i].g, &d->dbcc_classguid, sizeof(GUID)) == 0) sub = ue_classes[i].sub;
    ue_guid_text(&d->dbcc_classguid, cls, sizeof cls);
    int len = snprintf(ev, sizeof ev, "%s@%s\nACTION=%s\nDEVPATH=%s\nSUBSYSTEM=%s\nDEVICE_CLASS=%s\n",
                       action, path, action, path, sub, cls);
    /* USB ids in Linux's PRODUCT form (hex, no padding): "403/6001/0". */
    char low[600];
    size_t i = 0;
    for (; path[i] && i < sizeof low - 1; i++)
        low[i] = (char)(path[i] >= 'A' && path[i] <= 'Z' ? path[i] + 32 : path[i]);
    low[i] = 0;
    const char *v = strstr(low, "vid_"), *p = strstr(low, "pid_");
    if (v && p && len > 0 && len < (int)sizeof ev - 32) {
        unsigned vid = (unsigned)strtoul(v + 4, NULL, 16) & 0xFFFF;
        unsigned pid = (unsigned)strtoul(p + 4, NULL, 16) & 0xFFFF;
        snprintf(ev + len, sizeof ev - (size_t)len, "PRODUCT=%x/%x/0\n", vid, pid);
    }
    ue_push(m, ev);
}

static LRESULT CALLBACK ue_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UeMon *m = (UeMon *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (m && msg == WM_DEVICECHANGE && lp &&
        (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) {
        const DEV_BROADCAST_HDR *h = (const DEV_BROADCAST_HDR *)lp;
        if (h->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            ue_device(m, wp == DBT_DEVICEARRIVAL ? "add" : "remove",
                      (const DEV_BROADCAST_DEVICEINTERFACE_W *)lp);
        return TRUE;
    }
    if (m && msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE && lp) {
        const POWERBROADCAST_SETTING *ps = (const POWERBROADCAST_SETTING *)lp;
        if (ps->DataLength >= sizeof(DWORD)) {
            DWORD v;
            memcpy(&v, ps->Data, sizeof v);
            char ev[256];
            if (memcmp(&ps->PowerSetting, &ue_GUID_ACDC, sizeof(GUID)) == 0) {
                if (m->seen[0]++)
                    snprintf(ev, sizeof ev, "change@/power_supply/AC\nACTION=change\n"
                             "DEVPATH=/power_supply/AC\nSUBSYSTEM=power_supply\n"
                             "POWER_SUPPLY_NAME=AC\nPOWER_SUPPLY_ONLINE=%d\n", v == 0 ? 1 : 0),
                    ue_push(m, ev);
            } else if (memcmp(&ps->PowerSetting, &ue_GUID_BATPCT, sizeof(GUID)) == 0) {
                if (m->seen[1]++)
                    snprintf(ev, sizeof ev, "change@/power_supply/BAT0\nACTION=change\n"
                             "DEVPATH=/power_supply/BAT0\nSUBSYSTEM=power_supply\n"
                             "POWER_SUPPLY_NAME=BAT0\nPOWER_SUPPLY_CAPACITY=%lu\n",
                             (unsigned long)v),
                    ue_push(m, ev);
            }
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ue_teardown(UeMon *m) {
    for (int i = 0; i < 2; i++)
        if (m->pw[i]) { UnregisterPowerSettingNotification(m->pw[i]); m->pw[i] = NULL; }
    if (m->dev) { UnregisterDeviceNotification(m->dev); m->dev = NULL; }
    if (m->hwnd) { DestroyWindow(m->hwnd); m->hwnd = NULL; }
}

/* Runs on the pump thread: the window has to belong to the thread that pumps
   its messages, so it is made here, on the first call. */
static int ue_pump_read(void *ctx, char *buf, int cap, HANDLE stop, DWORD *err) {
    UeMon *m = (UeMon *)ctx;
    if (!m->hwnd) {
        static LONG registered;
        HINSTANCE inst = GetModuleHandleW(NULL);
        if (!InterlockedExchange(&registered, 1)) {
            WNDCLASSEXW wc;
            memset(&wc, 0, sizeof wc);
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = ue_wndproc;
            wc.hInstance = inst;
            wc.lpszClassName = L"VytoUEvent";
            RegisterClassExW(&wc);
        }
        m->hwnd = CreateWindowExW(0, L"VytoUEvent", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
        if (!m->hwnd) { *err = GetLastError(); return VT_PUMP_FAILED; }
        SetWindowLongPtrW(m->hwnd, GWLP_USERDATA, (LONG_PTR)m);
        DEV_BROADCAST_DEVICEINTERFACE_W f;
        memset(&f, 0, sizeof f);
        f.dbcc_size = sizeof f;
        f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        m->dev = RegisterDeviceNotificationW(m->hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE |
                                                         DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
        m->pw[0] = RegisterPowerSettingNotification(m->hwnd, &ue_GUID_ACDC, DEVICE_NOTIFY_WINDOW_HANDLE);
        m->pw[1] = RegisterPowerSettingNotification(m->hwnd, &ue_GUID_BATPCT, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!m->dev) { *err = GetLastError(); ue_teardown(m); return VT_PUMP_FAILED; }
    }
    for (;;) {
        if (m->qlen > 0) {
            int n = m->qlen < cap ? m->qlen : cap;
            memcpy(buf, m->q, (size_t)n);
            memmove(m->q, m->q + n, (size_t)(m->qlen - n));
            m->qlen -= n;
            return n;
        }
        DWORD r = MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) { ue_teardown(m); return VT_PUMP_STOPPED; }
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
}

int uevent_open(void) {
    int fd = -1;
    for (int i = 0; i < UE_SLOTS; i++) if (!g_ue[i]) { fd = i; break; }
    if (fd < 0) return -1;
    UeMon *m = (UeMon *)calloc(1, sizeof *m);
    if (!m) return -1;
    m->pump = vt_pump_start(ue_pump_read, m);
    if (!m->pump) { free(m); return -1; }
    g_ue[fd] = m;
    return fd;
}

int uevent_poll_fd(int fd) {
    UeMon *m = (fd >= 0 && fd < UE_SLOTS) ? g_ue[fd] : NULL;
    return m ? vt_pump_fd(m->pump) : -1;
}

/* One event: peek for its NUL, then take exactly that much, so the rest stay
   in the socket and keep it readable. >0 bytes, -1 none complete yet, -2 the
   monitor is dead. */
int64_t uevent_read(int fd, char *buf, int64_t cap) {
    UeMon *m = (fd >= 0 && fd < UE_SLOTS) ? g_ue[fd] : NULL;
    if (!m || cap < 2) return -2;
    int c = cap > 65536 ? 65536 : (int)cap;
    int n = recv(m->pump->vy, buf, c - 1, MSG_PEEK);
    if (n == 0) return -2;
    if (n < 0) return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
    char *z = (char *)memchr(buf, 0, (size_t)n);
    if (!z) {
        if (n < c - 1) return -1;            /* the rest is still on its way */
        /* longer than the buffer: drop it rather than wedge the stream */
        recv(m->pump->vy, buf, n, 0);
        return -1;
    }
    int len = (int)(z - buf);
    recv(m->pump->vy, buf, len + 1, 0);
    buf[len] = 0;
    return len;
}

void uevent_close(int fd) {
    UeMon *m = (fd >= 0 && fd < UE_SLOTS) ? g_ue[fd] : NULL;
    if (!m) return;
    /* The window's thread notices the stop event in MsgWaitForMultipleObjects
       and destroys the window itself; nothing to cancel. */
    vt_pump_stop(m->pump, NULL);
    free(m);
    g_ue[fd] = NULL;
}

#else /* other platforms: no device events */

int  uevent_open(void) { return -1; }
int64_t uevent_read(int fd, char *buf, int64_t cap) { (void)fd; (void)buf; (void)cap; return -2; }
void uevent_close(int fd) { (void)fd; }
int uevent_poll_fd(int fd) { (void)fd; return -1; }

#endif
