/* vyto/desktop/channel — the few OS pieces the channel needs that Vyto's
 * stdlib does not have: where discovery files live, writing one atomically
 * with the right readers, and whether a process id is still alive.
 * See ../../channel.vt.
 *
 * Every Windows function used is in the audited Windows 7 export list. */
#include <stdint.h>

#if defined(_WIN32) && !defined(VT_NO_LIBC)

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vyto_win.h"

static char g_out[4096];

/* Candidate base dirs, '\n'-separated: $VYTO_CHANNEL_DIR, else %ProgramData%. */
const char *vch_dirs(void) {
    wchar_t ev[1024];
    DWORD n = GetEnvironmentVariableW(L"VYTO_CHANNEL_DIR", ev, 1024);
    if (n > 0 && n < 1024) {
        char *u = vt_utf8_from_w(ev);
        snprintf(g_out, sizeof g_out, "%s", u ? u : "");
        free(u);
        return g_out;
    }
    PWSTR p = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &p)) || !p) return "";
    char *u = vt_utf8_from_w(p);
    CoTaskMemFree(p);
    snprintf(g_out, sizeof g_out, "%s", u ? u : "");
    free(u);
    return g_out;
}

/* mkdir -p; 1 when `path` is a directory afterwards. */
int32_t vch_prepare_dir(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 3; *p; p++) {
        if (*p != '\\' && *p != '/') continue;
        char c = *p;
        *p = 0;
        VtWBuf w;
        vt_wbuf_from(&w, buf);
        CreateDirectoryW(w.p, NULL);
        vt_wbuf_free(&w);
        *p = c;
    }
    VtWBuf w;
    vt_wbuf_from(&w, buf);
    CreateDirectoryW(w.p, NULL);
    DWORD a = GetFileAttributesW(w.p);
    vt_wbuf_free(&w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* Write `text` to `path` atomically (a temp file renamed over it), readable
   by signed-in users and writable only by SYSTEM and Administrators — the
   discovery file holds the channel's token. */
int32_t vch_write(const char *path, const char *text, int32_t len) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp%lu", path, (unsigned long)GetCurrentProcessId());
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;AU)", SDDL_REVISION_1, &sd, NULL))
        return 0;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    VtWBuf wt, wp;
    vt_wbuf_from(&wt, tmp);
    vt_wbuf_from(&wp, path);
    int ok = 0;
    HANDLE h = CreateFileW(wt.p, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        ok = WriteFile(h, text, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
        CloseHandle(h);
        if (ok) ok = MoveFileExW(wt.p, wp.p, MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) DeleteFileW(wt.p);
    }
    LocalFree(sd);
    vt_wbuf_free(&wt);
    vt_wbuf_free(&wp);
    return ok;
}

int32_t vch_pid(void) { return (int32_t)GetCurrentProcessId(); }

int32_t vch_pid_alive(int32_t pid) {
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   /* exists, not ours to open */
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

#elif !defined(VT_NO_LIBC)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_out[4096];

/* Candidates, '\n'-separated, in the order servers and clients both use:
   $VYTO_CHANNEL_DIR; /run/vyto (system services); $XDG_RUNTIME_DIR/vyto
   (the user's session); /tmp/vyto-<uid>. */
const char *vch_dirs(void) {
    const char *ev = getenv("VYTO_CHANNEL_DIR");
    if (ev && *ev) { snprintf(g_out, sizeof g_out, "%s", ev); return g_out; }
    const char *xr = getenv("XDG_RUNTIME_DIR");
    int n = snprintf(g_out, sizeof g_out, "/run/vyto");
    if (xr && *xr && xr[0] == '/') n += snprintf(g_out + n, sizeof g_out - (size_t)n, "\n%s/vyto", xr);
    snprintf(g_out + n, sizeof g_out - (size_t)n, "\n/tmp/vyto-%u", (unsigned)getuid());
    return g_out;
}

/* mkdir -p, then insist the leaf is a real directory (not a symlink) owned
   by us or root and not writable by others — a shared /tmp must not let
   another user plant the directory our token goes into. */
int32_t vch_prepare_dir(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
    struct stat st;
    if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    if (st.st_uid != getuid() && st.st_uid != 0) return 0;
    if (st.st_mode & S_IWOTH) return 0;
    return access(buf, W_OK) == 0;
}

int32_t vch_write(const char *path, const char *text, int32_t len) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    fchmod(fd, 0644);   /* not narrowed by the umask: GUI clients run as other users */
    ssize_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, (size_t)(len - off));
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return 0; }
        off += w;
    }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

int32_t vch_pid(void) { return (int32_t)getpid(); }

int32_t vch_pid_alive(int32_t pid) {
    if (pid <= 0) return 0;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

#else /* freestanding */

const char *vch_dirs(void) { return ""; }
int32_t vch_prepare_dir(const char *path) { (void)path; return 0; }
int32_t vch_write(const char *path, const char *text, int32_t len) { (void)path; (void)text; (void)len; return 0; }
int32_t vch_pid(void) { return 0; }
int32_t vch_pid_alive(int32_t pid) { (void)pid; return 0; }

#endif
