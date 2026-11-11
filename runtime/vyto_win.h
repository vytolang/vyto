/* vyto_win.h — UTF-8 <-> UTF-16 for the Windows arms of the runtime and of
 * package shims.
 *
 * Vyto strings are UTF-8 everywhere. The Windows "A" APIs and msvcrt's narrow
 * fopen/stat/getenv read bytes in the ANSI code page instead, so a path like
 * C:\Users\Zoë fails on every Windows that is not Win10 1903+ with the
 * manifest's UTF-8 code page (and Windows 7 has no such thing). The fix is to
 * convert at the boundary and call the "W" API. This header is the one place
 * that conversion lives; shims reach it through the -I<runtime dir> vytoc adds
 * to every package compile.
 *
 * Header-only and static inline so a shim needs nothing at link time. Off
 * Windows only vt_fopen() exists, as plain fopen, so shared code can call one
 * name on every target.
 */
#ifndef VYTO_WIN_H
#define VYTO_WIN_H

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* UTF-8 -> UTF-16 into a stack buffer, spilling to the heap for long text.
   MAX_PATH-sized, so an ordinary path never allocates. Always
   NUL-terminated when it succeeds. */
typedef struct {
    wchar_t *p;
    int n;               /* UTF-16 units, excluding the NUL */
    wchar_t stack[260];
} VtWBuf;

/* len < 0 means NUL-terminated. Returns 1 on success; on failure p is an
   empty string, so a caller that ignores the result passes "" rather than
   garbage. */
static inline int vt_wbuf_from_n(VtWBuf *b, const char *s, int len) {
    b->p = b->stack;
    b->n = 0;
    b->stack[0] = 0;
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    if (len == 0) return 1;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, b->stack, 259);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
        if (n <= 0) return 0;
        b->p = (wchar_t *)malloc(sizeof(wchar_t) * ((size_t)n + 1));
        if (!b->p) { b->p = b->stack; return 0; }
        n = MultiByteToWideChar(CP_UTF8, 0, s, len, b->p, n);
        if (n <= 0) { free(b->p); b->p = b->stack; return 0; }
    }
    b->p[n] = 0;
    b->n = n;
    return 1;
}

static inline int vt_wbuf_from(VtWBuf *b, const char *s) {
    return vt_wbuf_from_n(b, s, -1);
}

static inline void vt_wbuf_free(VtWBuf *b) {
    if (b->p != b->stack) free(b->p);
    b->p = b->stack;
}

/* UTF-16 (NUL-terminated, or n units when n >= 0) -> malloc'd UTF-8. NULL
   only when out of memory; an empty or NULL input gives "". */
static inline char *vt_utf8_from_wn(const wchar_t *w, int n) {
    if (!w) n = 0;
    else if (n < 0) n = (int)wcslen(w);
    int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL) : 0;
    char *out = (char *)malloc((size_t)(len > 0 ? len : 0) + 1);
    if (!out) return NULL;
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, out, len, NULL, NULL);
    out[len > 0 ? len : 0] = 0;
    return out;
}

static inline char *vt_utf8_from_w(const wchar_t *w) {
    return vt_utf8_from_wn(w, -1);
}

/* fopen with a UTF-8 path. The mode is ASCII, so a fixed buffer does. */
static inline FILE *vt_fopen(const char *path, const char *mode) {
    VtWBuf p;
    wchar_t m[16];
    int i = 0;
    for (; mode[i] && i < 15; i++) m[i] = (wchar_t)(unsigned char)mode[i];
    m[i] = 0;
    if (!vt_wbuf_from(&p, path)) { vt_wbuf_free(&p); return NULL; }
    FILE *f = _wfopen(p.p, m);
    vt_wbuf_free(&p);
    return f;
}

#else  /* not Windows */

#define vt_fopen(path, mode) fopen((path), (mode))

#endif
#endif /* VYTO_WIN_H */
