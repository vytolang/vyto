/* Does vyto/terminal put the terminal back when the process is killed?
 *
 * This is a C harness rather than a .vt example because the property under
 * test is about a process DYING — by a signal, or through exit(101) — and
 * reading the tty state AFTERWARDS. A Vyto program cannot observe its own
 * corpse, and the examples harness diffs stdout from a process that exited
 * normally, so neither can express this.
 *
 * Why it is worth a test at all: the three exit paths were each found broken
 * by measurement, in order.
 *
 *   deinit alone  -> panic() is exit(101), unwinds nothing     (tty at -echo)
 *   + atexit      -> SIGINT never runs atexit handlers  (tty at -echo -icanon)
 *   + handlers    -> correct
 *
 * Ctrl+C is how people quit an interactive program, so the third is the common
 * case, not an edge one. A regression here is invisible in normal use — every
 * test still passes, every program still works — right up until a user hits
 * Ctrl+C and their shell goes blind.
 *
 * Built and run by tests/run_tests.sh, which supplies the pty.
 *
 *   cc -o sigcheck terminal_signal_check.c ../../lib/vyto/terminal/native/src/term_shim.c
 *   ./sigcheck <case>
 *
 * Cases: raw-then-signal (child), and the guard checks which run inline.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

void *vterm_raw(int fd);
void  vterm_restore(void *h);

static void app_handler(int s) { (void)s; }

/* Enter raw mode and sit there until something kills us. The parent checks the
   tty afterwards. */
static int case_hold(void) {
    void *h = vterm_raw(0);
    if (!h) { fprintf(stderr, "no tty\n"); return 2; }
    printf("ready\n");
    fflush(stdout);
    for (;;) pause();
    /* not reached */
}

/* The shim must not stamp on a handler the application already installed, and
   must take its own back off when raw mode ends — otherwise a program that
   uses the prompt once is left with the library's SIGINT handling for the rest
   of its life. */
static int case_guard(void) {
    struct sigaction sa, got;
    void *h;
    int bad = 0;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = app_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, 0);          /* the app owns SIGINT */
    /* SIGTERM deliberately left at SIG_DFL */

    h = vterm_raw(0);
    if (!h) { fprintf(stderr, "no tty\n"); return 2; }

    sigaction(SIGINT, 0, &got);
    if (got.sa_handler != app_handler) { printf("BAD app SIGINT overwritten\n"); bad = 1; }

    sigaction(SIGTERM, 0, &got);
    if (got.sa_handler == SIG_DFL) { printf("BAD SIGTERM not hooked\n"); bad = 1; }

    vterm_restore(h);

    sigaction(SIGTERM, 0, &got);
    if (got.sa_handler != SIG_DFL) { printf("BAD SIGTERM still hooked after restore\n"); bad = 1; }

    sigaction(SIGINT, 0, &got);
    if (got.sa_handler != app_handler) { printf("BAD app SIGINT lost after restore\n"); bad = 1; }

    if (!bad) printf("guard ok\n");
    return bad;
}

/* Raw mode, then leave it the ordinary way. The tty must be back to normal
   while the process is still alive. */
static int case_normal(void) {
    struct termios before, after;
    void *h;
    if (tcgetattr(0, &before) != 0) { fprintf(stderr, "no tty\n"); return 2; }
    h = vterm_raw(0);
    if (!h) { fprintf(stderr, "no tty\n"); return 2; }
    vterm_restore(h);
    if (tcgetattr(0, &after) != 0) return 2;
    if ((before.c_lflag & (ECHO | ICANON)) != (after.c_lflag & (ECHO | ICANON))) {
        printf("BAD lflag not restored\n");
        return 1;
    }
    printf("normal ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s hold|guard|normal\n", argv[0]); return 2; }
    if (!strcmp(argv[1], "hold"))   return case_hold();
    if (!strcmp(argv[1], "guard"))  return case_guard();
    if (!strcmp(argv[1], "normal")) return case_normal();
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
