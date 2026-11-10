/* vyto/terminal native backing — controlling-terminal state and key input.

   Opaque handle + explicit free, the mmap_shim.c convention: vterm_raw is the
   constructor and returns NULL on failure, vterm_restore is the single free,
   every other call returns 0 on success and negative on failure. The handle
   carries the saved terminal state, so the Vyto side never learns which
   platform it is on.

   vytoc compiles every C file under native/src for every target with no
   per-file filter (CLAUDE.md), so the whole file is bracketed three ways: a
   freestanding stub arm, then Win32, then POSIX.

   ── KEY DECODING LIVES HERE, NOT IN VYTO ────────────────────────────────────
   An arrow key arrives on a POSIX terminal as three bytes (ESC [ A) and on
   Win32 as a single KEY_EVENT_RECORD with a virtual-key code. Those have
   nothing in common. Decoding in C means vterm_read_key returns one key code
   per call on both, so terminal.vt carries no platform conditionals and the
   codes can match vyto/surface's numbering exactly (printable characters are
   their own byte; specials start at 1000). That shared numbering is what lets
   a future vyto/tui consume either source with no translation table.

   ── THE RESTORE IS THE WHOLE POINT, AND deinit IS NOT ENOUGH ────────────────
   Raw mode disables echo. A program that exits without restoring leaves the
   user typing blind into a shell that shows nothing — they have to run `reset`
   from memory, with no feedback while they type it.

   The Vyto side ties vterm_restore to a deinit, which covers every *normal*
   exit path. It does not cover panic(): vt_panic_c calls vt_host_abort(),
   which is exit(101) (runtime/vyto_rt.c:553), and that unwinds nothing — no
   destructor runs. Measured: a panic in raw mode left the tty at -echo.

   So the handle is ALSO registered with atexit(), which exit(101) does honour.
   That makes the restore idempotent by necessity — it runs once from deinit
   and once more at exit, or only at exit after a panic — so vterm_restore
   clears the saved flag and the atexit hook only acts on a handle still
   holding state. A live handle is tracked in a file-static because atexit
   takes no argument; one terminal per process makes that sound.

   atexit is still not enough, because the most common way to quit an
   interactive program does not go through exit() at all. ISIG is left on
   deliberately (a tool that swallows Ctrl+C is a trap), so Ctrl+C raises
   SIGINT, whose default action terminates the process without running atexit
   handlers — measured leaving the tty at -echo -icanon, which is exactly the
   blind shell this is all meant to prevent.

   So the POSIX arm also installs handlers for the terminating signals a
   terminal program actually receives: SIGINT, SIGTERM, SIGQUIT and SIGHUP.
   Each restores the terminal, reinstates the previous disposition, and
   re-raises, so the process still dies with the right status and the
   application still sees the signal it expected. Handlers are installed ONLY
   where the app has not already set one (the previous handler is SIG_DFL), so
   this never stamps on an app's own handling — which was the reason to think
   twice about doing it in a library at all.

   SIGKILL and SIGSTOP remain uncoverable by anything, by design. */

/* Key codes. These MUST match VS_KEY_* in lib/vyto/surface/vsurf.vt — a key is
   the same key whether it came from X11 or from a tty. */
#define VTERM_KEY_ENTER     1000
#define VTERM_KEY_BACKSPACE 1001
#define VTERM_KEY_ESC       1002
#define VTERM_KEY_UP        1003
#define VTERM_KEY_DOWN      1004
#define VTERM_KEY_LEFT      1005
#define VTERM_KEY_RIGHT     1006
#define VTERM_KEY_DELETE    1007
#define VTERM_KEY_TAB       1008
#define VTERM_KEY_HOME      1009
#define VTERM_KEY_END       1010
#define VTERM_KEY_PAGEUP    1011
#define VTERM_KEY_PAGEDOWN  1012
#define VTERM_KEY_INSERT    1013

/* Non-key results from vterm_read_key. Negative so no key code collides. */
#define VTERM_EOF     (-1)   /* stdin closed */
#define VTERM_TIMEOUT (-2)   /* nothing to read before timeout_ms elapsed */
#define VTERM_ERR     (-3)

/* ------------------------------------------------------------------------ */
#ifdef VT_NO_LIBC
/* Freestanding: there is no terminal, and no libc to ask about one.
   --freestanding splices -DVT_NO_LIBC into the package shim's compile line, so
   an unguarded <termios.h> would break the build of any program that merely
   imports vyto/terminal. Keep every symbol so such a program still links, and
   make each one report "not a terminal" — which is a state the hosted arms
   reach too (a pipe), so terminal.vt needs no platform conditional and its
   isatty()==false path is the one already exercised by the test suite.

   Deliberately no #include at all, not even <stddef.h>, so this arm cannot
   regress into depending on a hosted header. */

void *vterm_raw(int fd)                    { (void)fd; return 0; }
void  vterm_restore(void *h)               { (void)h; }
int   vterm_read_key(void *h, int ms)      { (void)h; (void)ms; return VTERM_EOF; }
int   vterm_size(void *out)                { (void)out; return -1; }
int   vterm_isatty(int fd)                 { (void)fd; return 0; }
int   vterm_styled(void)                   { return 0; }
int   vterm_tty_out(void)                  { return 0; }
long long vterm_write(int fd, const char *b, long long n)
                                           { (void)fd; (void)b; (void)n; return -1; }

/* ------------------------------------------------------------------------ */
#else

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int vterm_isatty(int fd);

/* Should ANSI styling be emitted? Computed once, then cached.

   Cached because every styling helper in ansi.vt consults it, so a redraw
   emitting a few hundred styled spans was making a few hundred isatty(2) calls
   per frame. Measured over 200k paint() calls: 2.81 us each uncached, 0.27 us
   cached — 10x. None of the three inputs can change during the process: a
   descriptor does not become a terminal, and the environment is read at
   startup.

   It lives here rather than in ansi.vt because Vyto has no module-level `let`
   and no static class fields (lib/vyto/dev/test/test.vt:42), so a pure-Vyto
   module has nowhere to keep a cache.

   Gated on STDOUT, never stdin: `prog | less` has an interactive stdin and a
   piped stdout, and deciding on stdin would write escapes into the pipe. */
/* Is stdout a terminal? Cached for the same reason, and split out because
   cursor movement and clearing must stay available when NO_COLOR is set:
   those are layout, not decoration. */
int vterm_tty_out(void) {
    static int cached = -1;
    if (cached < 0) cached = vterm_isatty(1) ? 1 : 0;
    return cached;
}

int vterm_styled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *nc = getenv("NO_COLOR");     /* no-color.org: any value */
        const char *term = getenv("TERM");
        if (nc && *nc)                           cached = 0;
        else if (term && strcmp(term, "dumb") == 0) cached = 0;
        else                                     cached = vterm_tty_out();
    }
    return cached;
}

#ifdef _WIN32
/* ---------------------------------------------------------------- Win32 --
   A console is not a tty: there is no termios, and input arrives as
   INPUT_RECORDs rather than bytes. SetConsoleMode with ENABLE_LINE_INPUT and
   ENABLE_ECHO_INPUT cleared is the raw-mode equivalent, and ReadConsoleInput
   hands back a virtual-key code that maps straight onto our numbering with no
   escape-sequence parsing at all. */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <io.h>

typedef struct { HANDLE in; DWORD saved; int live; } VtTerm;

/* See the header: exit(101) from a panic runs atexit hooks but no destructor,
   so the restore cannot rely on the Vyto deinit alone. */
static VtTerm *g_live;

static void vterm_atexit(void) {
    VtTerm *t = g_live;
    if (t && t->live) { SetConsoleMode(t->in, t->saved); t->live = 0; }
}

void *vterm_raw(int fd) {
    HANDLE in = (HANDLE)_get_osfhandle(fd);
    DWORD mode;
    VtTerm *t;
    static int hooked;
    if (in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode)) return 0;
    t = (VtTerm *)calloc(1, sizeof *t);
    if (!t) return 0;
    t->in = in; t->saved = mode; t->live = 1;
    /* Drop line buffering and echo; keep the window-resize notifications so a
       later vterm_size reflects reality. */
    mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(in, mode)) { free(t); return 0; }

    g_live = t;
    if (!hooked) { hooked = 1; atexit(vterm_atexit); }
    return t;
}

void vterm_restore(void *h) {
    VtTerm *t = (VtTerm *)h;
    if (!t) return;
    if (t->live) { SetConsoleMode(t->in, t->saved); t->live = 0; }
    if (g_live == t) g_live = 0;
    free(t);
}

static int vterm_vk(WORD vk, WCHAR ch) {
    switch (vk) {
        case VK_RETURN: return VTERM_KEY_ENTER;
        case VK_BACK:   return VTERM_KEY_BACKSPACE;
        case VK_ESCAPE: return VTERM_KEY_ESC;
        case VK_UP:     return VTERM_KEY_UP;
        case VK_DOWN:   return VTERM_KEY_DOWN;
        case VK_LEFT:   return VTERM_KEY_LEFT;
        case VK_RIGHT:  return VTERM_KEY_RIGHT;
        case VK_DELETE: return VTERM_KEY_DELETE;
        case VK_TAB:    return VTERM_KEY_TAB;
        case VK_HOME:   return VTERM_KEY_HOME;
        case VK_END:    return VTERM_KEY_END;
        case VK_PRIOR:  return VTERM_KEY_PAGEUP;
        case VK_NEXT:   return VTERM_KEY_PAGEDOWN;
        case VK_INSERT: return VTERM_KEY_INSERT;
        default: break;
    }
    /* Printable: the character is its own code, matching the POSIX arm. Only
       ASCII is reported; a wide character has no single-byte code and is
       skipped rather than truncated into a wrong one. */
    if (ch > 0 && ch < 128) return (int)ch;
    return VTERM_TIMEOUT;   /* "nothing usable"; caller loops */
}

int vterm_read_key(void *h, int timeout_ms) {
    VtTerm *t = (VtTerm *)h;
    INPUT_RECORD rec;
    DWORD got;
    if (!t) return VTERM_ERR;
    for (;;) {
        DWORD w = WaitForSingleObject(t->in, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        if (w == WAIT_TIMEOUT) return VTERM_TIMEOUT;
        if (w != WAIT_OBJECT_0) return VTERM_ERR;
        if (!ReadConsoleInputW(t->in, &rec, 1, &got) || got == 0) return VTERM_EOF;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            int k = vterm_vk(rec.Event.KeyEvent.wVirtualKeyCode,
                             rec.Event.KeyEvent.uChar.UnicodeChar);
            if (k != VTERM_TIMEOUT) return k;
        }
        /* A key-up, a resize or a mouse record is not an error — go round
           again rather than reporting a phantom timeout. */
    }
}

int vterm_size(void *out) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int *wh = (int *)out;
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!wh || o == INVALID_HANDLE_VALUE) return -1;
    if (!GetConsoleScreenBufferInfo(o, &csbi)) return -1;
    wh[0] = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
    wh[1] = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    if (wh[0] <= 0 || wh[1] <= 0) return -1;
    return 0;
}

int vterm_isatty(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD mode;
    if (h == INVALID_HANDLE_VALUE) return 0;
    return GetConsoleMode(h, &mode) ? 1 : 0;
}

long long vterm_write(int fd, const char *buf, long long n) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD wrote = 0;
    if (h == INVALID_HANDLE_VALUE || n < 0) return -1;
    fflush(0);   /* see the POSIX arm: print() and this do not share a buffer */
    if (!WriteFile(h, buf, (DWORD)n, &wrote, 0)) return -1;
    return (long long)wrote;
}

#else
/* ---------------------------------------------------------------- POSIX --
   tcgetattr saves, cfmakeraw-equivalent flags clear ICANON and ECHO, tcsetattr
   applies. The same shape as the fbdev backend does inline at
   lib/vyto/surface/native/src/vsurf.c:1652, lifted out so it is reusable and
   so the restore is tied to a handle rather than to one backend's teardown. */

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

/* `live` is volatile sig_atomic_t, not int: a signal handler reads and writes
   it, so it must be a type the standard guarantees can be accessed atomically
   from one, and volatile so the compiler cannot cache it across the call that
   the signal interrupts. */
typedef struct { int fd; struct termios saved; volatile sig_atomic_t live; } VtTerm;

/* The handle currently in raw mode, for the atexit and signal hooks — neither
   takes an argument, so both have to find the state somewhere. A process has
   one controlling terminal, so a single slot is the right shape. */
static VtTerm *volatile g_live;

/* Restore, if it has not happened already. Async-signal-safe on purpose:
   tcsetattr is on the POSIX list, and there is no free() or stdio here. Never
   frees — at exit there is nothing to reclaim, and touching the allocator from
   a handler could deadlock against a signal that arrived inside malloc. */
static void vterm_undo(void) {
    VtTerm *h = g_live;
    if (h && h->live) {
        tcsetattr(h->fd, TCSANOW, &h->saved);
        h->live = 0;
    }
}

static void vterm_atexit(void) { vterm_undo(); }

/* The signals that terminate an interactive program. SIGINT is the one that
   matters — Ctrl+C is how people quit — but a terminal emulator closing
   (SIGHUP) or a `kill` (SIGTERM) leave the tty just as broken. */
static const int VTERM_SIGS[4] = { SIGINT, SIGTERM, SIGQUIT, SIGHUP };
static struct sigaction vterm_prev[4];
/* Written by the handler as well as by the installer — see `live` above. */
static volatile sig_atomic_t vterm_hooked_sig[4];

static void vterm_on_signal(int sig) {
    int i;
    vterm_undo();
    /* Put back what was there and re-raise, so the process dies with the
       status the caller expects (killed-by-SIGINT, not exit(0)) and any
       handler the app installs later still behaves. */
    for (i = 0; i < 4; i++) {
        if (VTERM_SIGS[i] == sig && vterm_hooked_sig[i]) {
            sigaction(sig, &vterm_prev[i], 0);
            vterm_hooked_sig[i] = 0;
            break;
        }
    }
    raise(sig);
}

/* Install handlers only where the application has none. Stamping on an app's
   own SIGINT handling would be worse than the problem being solved. */
static void vterm_hook_signals(void) {
    struct sigaction sa;
    int i;
    for (i = 0; i < 4; i++) {
        if (vterm_hooked_sig[i]) continue;
        if (sigaction(VTERM_SIGS[i], 0, &vterm_prev[i]) != 0) continue;
        if (vterm_prev[i].sa_handler != SIG_DFL) continue;   /* app owns it */
        sa = vterm_prev[i];
        sa.sa_handler = vterm_on_signal;
        sa.sa_flags &= ~SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        if (sigaction(VTERM_SIGS[i], &sa, 0) == 0) vterm_hooked_sig[i] = 1;
    }
}

/* Take the handlers back off, so a program that leaves raw mode and keeps
   running is exactly as it was before. */
static void vterm_unhook_signals(void) {
    int i;
    for (i = 0; i < 4; i++) {
        if (!vterm_hooked_sig[i]) continue;
        sigaction(VTERM_SIGS[i], &vterm_prev[i], 0);
        vterm_hooked_sig[i] = 0;
    }
}

void *vterm_raw(int fd) {
    struct termios t;
    VtTerm *h;
    static int hooked;
    if (!isatty(fd)) return 0;
    if (tcgetattr(fd, &t) != 0) return 0;
    h = (VtTerm *)calloc(1, sizeof *h);
    if (!h) return 0;
    h->fd = fd;
    h->saved = t;
    h->live = 1;

    /* Character-at-a-time, no echo. ISIG is deliberately LEFT ON so Ctrl+C
       still signals: a beginner-facing tool that traps Ctrl+C is a trap, and
       the restore happens on the way out regardless. */
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) { free(h); return 0; }

    g_live = h;
    if (!hooked) { hooked = 1; atexit(vterm_atexit); }
    vterm_hook_signals();
    return h;
}

void vterm_restore(void *p) {
    VtTerm *h = (VtTerm *)p;
    if (!h) return;
    if (h->live) { tcsetattr(h->fd, TCSANOW, &h->saved); h->live = 0; }
    if (g_live == h) { g_live = 0; vterm_unhook_signals(); }
    free(h);
}

/* Read one byte, honouring the timeout. Returns 1 on a byte, 0 on timeout,
   negative at EOF or on error. */
static int read_byte(int fd, int timeout_ms, unsigned char *out) {
    struct pollfd p;
    int r;
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    do { r = poll(&p, 1, timeout_ms); } while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    if (r < 0) return -1;
    do { r = (int)read(fd, out, 1); } while (r < 0 && errno == EINTR);
    if (r == 1) return 1;
    return -1;   /* 0 is EOF */
}

/* After an ESC, decode the CSI/SS3 sequence that follows.

   The 20ms budget is what separates a real Escape keypress from the start of
   an arrow key: both begin with 0x1b, and the only difference is that a
   terminal sends the rest of an arrow sequence immediately while a human
   cannot. Too short and arrows register as Escape over ssh; too long and
   pressing Escape feels sticky. */
static int decode_escape(int fd) {
    unsigned char b;
    int r = read_byte(fd, 20, &b);
    if (r <= 0) return VTERM_KEY_ESC;          /* a bare Escape */

    if (b == 'O') {                            /* SS3: some terminals' arrows */
        if (read_byte(fd, 20, &b) <= 0) return VTERM_KEY_ESC;
        switch (b) {
            case 'A': return VTERM_KEY_UP;
            case 'B': return VTERM_KEY_DOWN;
            case 'C': return VTERM_KEY_RIGHT;
            case 'D': return VTERM_KEY_LEFT;
            case 'H': return VTERM_KEY_HOME;
            case 'F': return VTERM_KEY_END;
            default:  return VTERM_KEY_ESC;
        }
    }
    if (b != '[') return VTERM_KEY_ESC;        /* Alt+key; not ours to report */

    if (read_byte(fd, 20, &b) <= 0) return VTERM_KEY_ESC;
    switch (b) {
        case 'A': return VTERM_KEY_UP;
        case 'B': return VTERM_KEY_DOWN;
        case 'C': return VTERM_KEY_RIGHT;
        case 'D': return VTERM_KEY_LEFT;
        case 'H': return VTERM_KEY_HOME;
        case 'F': return VTERM_KEY_END;
        default: break;
    }
    if (b >= '0' && b <= '9') {
        /* Numeric form: ESC [ N ~ . Consume to the terminator so a trailing
           '~' is never mistaken for a typed character. */
        int n = b - '0';
        unsigned char c;
        while (read_byte(fd, 20, &c) > 0) {
            if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); continue; }
            break;   /* '~' or a modifier ';' section */
        }
        switch (n) {
            case 1: case 7: return VTERM_KEY_HOME;
            case 2:         return VTERM_KEY_INSERT;
            case 3:         return VTERM_KEY_DELETE;
            case 4: case 8: return VTERM_KEY_END;
            case 5:         return VTERM_KEY_PAGEUP;
            case 6:         return VTERM_KEY_PAGEDOWN;
            default:        return VTERM_KEY_ESC;
        }
    }
    return VTERM_KEY_ESC;
}

int vterm_read_key(void *p, int timeout_ms) {
    VtTerm *h = (VtTerm *)p;
    unsigned char b;
    int r;
    if (!h) return VTERM_ERR;
    r = read_byte(h->fd, timeout_ms, &b);
    if (r == 0) return VTERM_TIMEOUT;
    if (r < 0)  return VTERM_EOF;

    if (b == 27)  return decode_escape(h->fd);
    if (b == 13 || b == 10) return VTERM_KEY_ENTER;
    /* Both are Backspace in the wild: DEL is what most terminals send, and ^H
       is what the rest send. Neither means "delete forwards" here. */
    if (b == 127 || b == 8) return VTERM_KEY_BACKSPACE;
    if (b == 9)   return VTERM_KEY_TAB;
    return (int)b;
}

int vterm_size(void *out) {
    struct winsize ws;
    int *wh = (int *)out;
    if (!wh) return -1;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return -1;
    if (ws.ws_col == 0 || ws.ws_row == 0) return -1;
    wh[0] = (int)ws.ws_col;
    wh[1] = (int)ws.ws_row;
    return 0;
}

int vterm_isatty(int fd) { return isatty(fd) ? 1 : 0; }

long long vterm_write(int fd, const char *buf, long long n) {
    long long done = 0;
    if (n < 0) return -1;
    /* print() reaches stdout through the runtime's buffered fwrite
       (runtime/vyto_rt.c:138) while this is an unbuffered write(2) on the same
       descriptor. They do not share a buffer, so without this flush a prompt
       written here jumps ahead of the output it is meant to follow. The same
       hazard is documented at local/vytodba/repl.vt:36-39. */
    fflush(0);
    while (done < n) {
        ssize_t w = write(fd, buf + done, (size_t)(n - done));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) break;
        done += w;
    }
    return done;
}

#endif /* _WIN32 */
#endif /* VT_NO_LIBC */
