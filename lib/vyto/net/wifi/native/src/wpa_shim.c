/* vyto/net/wifi native backing — wpa_supplicant control-socket transport.

   WiFi control (scan, connect) is done by talking to the wpa_supplicant daemon over its
   per-interface AF_UNIX datagram control socket (/run/wpa_supplicant/<iface>) — the same
   channel wpa_cli uses. Far less code than raw nl80211, and it reuses the socket
   discipline, so vyto/net/wifi is mostly pure Vyto over this thin transport. The command
   protocol (SCAN, SCAN_RESULTS, ADD_NETWORK, ...) is plain text, parsed on the Vyto side.

   The local endpoint binds in the abstract namespace (leading NUL) so there is no temp
   file to clean up and it vanishes on close. Connecting needs access to the control
   socket (the `netdev` group, or root); a denied open is soft (-1). Linux. */

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

/* Open a datagram control socket connected to wpa_supplicant's `ctrl_path`. Returns the
   fd, or -1 (no daemon, or no permission). */
int wpa_open(const char *ctrl_path) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    /* bind a unique abstract-namespace local address (no filesystem cleanup) */
    static int ctr = 0;
    struct sockaddr_un local;
    memset(&local, 0, sizeof local);
    local.sun_family = AF_UNIX;
    char name[80];
    int nl = snprintf(name, sizeof name, "vyto-wpa-%d-%d", (int)getpid(), ctr++);
    local.sun_path[0] = 0; /* abstract */
    memcpy(local.sun_path + 1, name, (size_t)nl);
    socklen_t local_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
    if (bind(fd, (struct sockaddr *)&local, local_len) < 0) { close(fd); return -1; }

    struct sockaddr_un dest;
    memset(&dest, 0, sizeof dest);
    dest.sun_family = AF_UNIX;
    strncpy(dest.sun_path, ctrl_path, sizeof dest.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&dest, sizeof dest) < 0) { close(fd); return -1; }

    struct timeval tv;
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* Send a command, read the reply into `reply` (NUL-terminated). Bytes received (>=0),
   or -1 on error/timeout. */
long wpa_cmd(int fd, const char *cmd, char *reply, long cap) {
    if (send(fd, cmd, strlen(cmd), 0) < 0) return -1;
    long n = (long)recv(fd, reply, (size_t)cap - 1, 0);
    if (n < 0) { reply[0] = 0; return -1; }
    reply[n] = 0;
    return n;
}

void wpa_close(int fd) { if (fd >= 0) close(fd); }

/* Open a *monitor* connection: a second control socket that ATTACHes to the daemon so
   wpa_supplicant pushes unsolicited events (CTRL-EVENT-CONNECTED / -DISCONNECTED /
   -SCAN-RESULTS, ...). Non-blocking, poll-able. Returns the fd, or -1. */
int wpa_attach(const char *ctrl_path) {
    int fd = wpa_open(ctrl_path);
    if (fd < 0) return -1;
    if (send(fd, "ATTACH", 6, 0) < 0) { close(fd); return -1; }
    char r[16];
    long n = (long)recv(fd, r, sizeof r - 1, 0); /* the "OK\n" ack (2s timeout) */
    if (n <= 0) { close(fd); return -1; }
    r[n] = 0;
    if (r[0] != 'O') { close(fd); return -1; } /* not OK -> ATTACH refused */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);        /* event reads are non-blocking */
    return fd;
}

/* Read one pushed event line into `buf` (NUL-terminated). >0 bytes, -1 would-block
   (nothing pending), -2 error. Read after the fd polls readable. */
long wpa_event_read(int fd, char *buf, long cap) {
    long n = (long)recv(fd, buf, (size_t)cap - 1, 0);
    if (n < 0) { buf[0] = 0; return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2; }
    buf[n] = 0;
    return n;
}

#else /* non-Linux: no wpa_supplicant socket */

int  wpa_open(const char *ctrl_path) { (void)ctrl_path; return -1; }
long wpa_cmd(int fd, const char *cmd, char *reply, long cap) { (void)fd; (void)cmd; (void)cap; if (cap > 0) reply[0] = 0; return -1; }
int  wpa_attach(const char *ctrl_path) { (void)ctrl_path; return -1; }
long wpa_event_read(int fd, char *buf, long cap) { (void)fd; (void)cap; if (cap > 0) buf[0] = 0; return -2; }
void wpa_close(int fd) { (void)fd; }

#endif
