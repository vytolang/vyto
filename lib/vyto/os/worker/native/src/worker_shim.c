/* vyto/os/worker native backing — pre-fork worker pool over socketpair.

   fork() gives real CPU parallelism with no shared state: each child is a full
   copy of the process that talks to the parent over a connected AF_UNIX
   socketpair. Messages are length-prefixed frames (4-byte big-endian length +
   payload) written/read in full by the loops below, so a job's input and result
   never tear across a partial read. The parent never packs a struct across FFI:
   socketpair fds come back as plain ints (the child fd is stashed for a paired
   accessor call, which is safe because the parent forks sequentially). */

#define _GNU_SOURCE
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>

/* Create a connected socketpair. Returns the parent-side fd (or -1); the
   child-side fd is read back with w_last_child_fd immediately after. Safe
   because the parent creates/forks pairs one at a time. */
static int g_last_child_fd = -1;
int w_socketpair(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    g_last_child_fd = sv[1];
    return sv[0];
}
int w_last_child_fd(void) { return g_last_child_fd; }

int  w_fork(void) { return (int)fork(); }
void w_close(int fd) { if (fd >= 0) close(fd); }

/* _exit (not exit) in the child: skip the parent's atexit/stdio-flush handlers,
   which would otherwise double-flush buffers inherited by the fork. */
void w_exit(int code) { _exit(code); }

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
/* 0 = filled, 1 = clean EOF, -1 = error. */
static int read_all(int fd, char *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return 1;
        if (r < 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* Write a length-prefixed frame. 0 ok, -1 error. */
int w_write_frame(int fd, const char *buf, int n) {
    if (n < 0) n = 0;
    uint32_t len = (uint32_t)n;
    unsigned char hdr[4] = { (unsigned char)(len >> 24), (unsigned char)(len >> 16),
                             (unsigned char)(len >> 8), (unsigned char)len };
    if (write_all(fd, (const char *)hdr, 4) != 0) return -1;
    if (n > 0 && write_all(fd, buf, (size_t)n) != 0) return -1;
    return 0;
}

/* Read the next frame's length. >=0 length, -1 = peer closed (EOF), -2 = error. */
int w_read_len(int fd) {
    unsigned char hdr[4];
    int r = read_all(fd, (char *)hdr, 4);
    if (r == 1) return -1;
    if (r < 0) return -2;
    return (int)(((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                 ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3]);
}
/* Read `n` payload bytes into buf (call right after w_read_len). 0 ok, -1 error. */
int w_read_bytes(int fd, char *buf, int n) {
    if (n <= 0) return 0;
    return read_all(fd, buf, (size_t)n) == 0 ? 0 : -1;
}

/* Terminate and reap a worker. Closing the parent fd already makes the child
   exit on EOF; the SIGTERM covers a child wedged in a long job. */
void w_reap(int pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}
