/* vyto_win_pump.h — make a Windows device HANDLE pollable, for vyto/hw shims.

   Every streaming device in vyto/hw hands Vyto a pollable fd, so it joins the
   same PollSet / window-loop wait as sockets. On Windows that wait is WSAPoll
   (or WSAEventSelect inside the Win32 window loop), and both accept SOCKETs
   only: a COM port, a HID device or a notification queue is a HANDLE and
   cannot be waited on there.

   The pump bridges it (local/docs/SERIAL_PORT_PROXY.md §2.1). Each open
   device gets one C thread that blocks in the device's own read and sends
   what arrives into a loopback TCP socket pair; Vyto polls and reads the
   other end like any socket. The device's bytes cost one extra copy through
   loopback, which is nothing at device rates, and an idle device costs no CPU.

   The thread never touches a Vyto object, so the single-threaded runtime is
   unaffected: everything it shares with the Vyto thread is this struct, whose
   mutable fields are written with Interlocked operations.

   Shape of a pump's life:
     - vt_pump_start(read_fn, ctx): pair + thread. read_fn runs on the thread
       and blocks until it has bytes, the stop event fires, or the device fails.
     - The Vyto side polls vt_pump_fd() and drains it with vt_pump_recv().
       0 from vt_pump_recv is EOF: the device failed (or was stopped), and
       vt_pump_error() says why.
     - vt_pump_stop(p, io): set the stop event, cancel the device's pending
       I/O, close the Vyto end (which fails a send the thread is blocked in),
       join the thread, free. The caller closes its device handle after.

   Header-only (static functions) so a shim needs no other package's symbols;
   vytoc folds this file into every shim's cache key, like vyto_win.h. Include
   it before <windows.h>: winsock2.h has to come first. */
#ifndef VYTO_WIN_PUMP_H
#define VYTO_WIN_PUMP_H

#if defined(_WIN32) && !defined(VT_NO_LIBC)

/* CancelIoEx and the rest are Vista. mingw defaults _WIN32_WINNT to XP the
   moment any CRT header is included, so a shim must ask for 0x0600 before
   its first #include; a later #define here would come too late. */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#error "define _WIN32_WINNT 0x0600 before the first #include in a shim using vyto_win_pump.h"
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* Called on the pump thread. Fill buf with up to cap bytes and return how
   many (> 0); 0 to be called again (a timeout with nothing read);
   VT_PUMP_STOPPED once `stop` is signalled; VT_PUMP_FAILED with *err set when
   the device failed (unplugged, I/O error). */
/* Stack reserved for a pump thread (address space only; pages are committed
   as used). */
#ifndef VT_PUMP_STACK
#define VT_PUMP_STACK (64 * 1024)
#endif

typedef int (*VtPumpReadFn)(void *ctx, char *buf, int cap, HANDLE stop, DWORD *err);
#define VT_PUMP_STOPPED (-1)
#define VT_PUMP_FAILED  (-2)

typedef struct VtPump {
    SOCKET vy;          /* Vyto's end: non-blocking, what pollFd() returns */
    SOCKET th;          /* the thread's end: blocking sends */
    HANDLE stop;        /* manual-reset event */
    HANDLE thread;
    VtPumpReadFn read;
    void *ctx;
    volatile LONG err;  /* the Win32 error that ended the pump; 0 = none */
} VtPump;

static inline int vt_pump_wsa(void) {
    static LONG done;
    if (done) return 1;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    InterlockedExchange(&done, 1);
    return 1;
}

/* A connected loopback pair. The listener is on 127.0.0.1 with an ephemeral
   port for the few microseconds this takes, and another local process could
   connect to it first; so the accepted socket must be the one whose peer is
   our own connecting socket, compared by port, or the pair is refused. */
static inline int vt_pump_pair(SOCKET *a, SOCKET *b) {
    if (!vt_pump_wsa()) return 0;
    SOCKET lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lis == INVALID_SOCKET) return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = (int)sizeof sa;
    BOOL excl = TRUE;
    setsockopt(lis, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (bind(lis, (struct sockaddr *)&sa, len) != 0 || listen(lis, 1) != 0 ||
        getsockname(lis, (struct sockaddr *)&sa, &len) != 0) {
        closesocket(lis);
        return 0;
    }
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET) { closesocket(lis); return 0; }
    if (connect(c, (struct sockaddr *)&sa, len) != 0) {
        closesocket(c);
        closesocket(lis);
        return 0;
    }
    SOCKET s = accept(lis, NULL, NULL);
    closesocket(lis);
    if (s == INVALID_SOCKET) { closesocket(c); return 0; }
    struct sockaddr_in mine, peer;
    int ml = (int)sizeof mine, pl = (int)sizeof peer;
    if (getsockname(c, (struct sockaddr *)&mine, &ml) != 0 ||
        getpeername(s, (struct sockaddr *)&peer, &pl) != 0 ||
        mine.sin_port != peer.sin_port) {
        closesocket(s);
        closesocket(c);
        return 0;
    }
    /* Small device chunks should go out at once, not wait for Nagle. */
    BOOL nd = TRUE;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
    *a = s;
    *b = c;
    return 1;
}

static inline int vt_pump_send_all(SOCKET s, const char *p, int n) {
    while (n > 0) {
        int w = send(s, p, n, 0);
        if (w == SOCKET_ERROR || w == 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static inline DWORD WINAPI vt_pump_main(LPVOID arg) {
    VtPump *p = (VtPump *)arg;
    char buf[4096];
    for (;;) {
        if (WaitForSingleObject(p->stop, 0) == WAIT_OBJECT_0) break;
        DWORD e = 0;
        int n = p->read(p->ctx, buf, (int)sizeof buf, p->stop, &e);
        if (n == VT_PUMP_STOPPED) break;
        if (n < 0) {
            InterlockedExchange(&p->err, (LONG)(e ? e : ERROR_GEN_FAILURE));
            break;
        }
        if (n > 0 && vt_pump_send_all(p->th, buf, n) != 0) break; /* Vyto end gone */
    }
    /* EOF on Vyto's end: what tells it the device is done. */
    shutdown(p->th, SD_SEND);
    return 0;
}

/* NULL on failure (no sockets, no thread). */
static inline VtPump *vt_pump_start(VtPumpReadFn fn, void *ctx) {
    VtPump *p = (VtPump *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->read = fn;
    p->ctx = ctx;
    if (!vt_pump_pair(&p->vy, &p->th)) { free(p); return NULL; }
    u_long nb = 1;
    ioctlsocket(p->vy, FIONBIO, &nb);
    p->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (p->stop) {
        p->thread = CreateThread(NULL, VT_PUMP_STACK, vt_pump_main, p,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    if (!p->thread) {
        if (p->stop) CloseHandle(p->stop);
        closesocket(p->vy);
        closesocket(p->th);
        free(p);
        return NULL;
    }
    return p;
}

static inline int vt_pump_fd(const VtPump *p) { return p ? (int)p->vy : -1; }

/* > 0 bytes, 0 EOF (the pump ended: see vt_pump_error), -1 nothing yet,
   -2 socket error. */
static inline int vt_pump_recv(VtPump *p, char *buf, int cap) {
    if (!p) return -2;
    int n = recv(p->vy, buf, cap, 0);
    if (n > 0) return n;
    if (n == 0) return 0;
    return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
}

static inline DWORD vt_pump_error(const VtPump *p) { return p ? (DWORD)p->err : 0; }

/* Stop and free. `io` is the device handle whose pending read must be
   cancelled (NULL if read_fn needs no cancel); the caller closes it after. */
static inline void vt_pump_stop(VtPump *p, HANDLE io) {
    if (!p) return;
    SetEvent(p->stop);
    if (io && io != INVALID_HANDLE_VALUE) CancelIoEx(io, NULL);
    closesocket(p->vy);           /* fails a send the thread is blocked in */
    WaitForSingleObject(p->thread, INFINITE);
    CloseHandle(p->thread);
    CloseHandle(p->stop);
    closesocket(p->th);
    free(p);
}

/* "The system cannot find the file specified." for a Win32 error, UTF-8,
   without the trailing CRLF. Writes "" when Windows has no text for it. */
static inline void vt_pump_errtext(DWORD e, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    WCHAR w[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             e, 0, w, 256, NULL);
    while (n > 0 && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L'.' ||
                     w[n - 1] == L' '))
        n--;
    if (n == 0) return;
    int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, cap - 1, NULL, NULL);
    out[k > 0 ? k : 0] = 0;
}

#endif /* _WIN32 && !VT_NO_LIBC */
#endif /* VYTO_WIN_PUMP_H */
