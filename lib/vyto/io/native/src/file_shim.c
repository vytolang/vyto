/* vyto/io/file native backing — thin wrappers over stdio + stat.

   Every buffer transfer is bounded by a length the Vyto side supplies (a
   `bytes(n)` array's capacity), and the actual byte count is returned so Vyto
   never reads past what was filled. The FILE* is an opaque `rawptr` owned by a
   Vyto `File` whose `deinit` calls vfile_close exactly once. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#include "vyto_win.h" /* UTF-8 paths through the W APIs on Windows */

/* One stat for both worlds. On Windows the narrow stat() reads the path in
   the ANSI code page (so C:\Users\Zoë fails) and its st_size is 32-bit, so
   go through _wstat64 instead. */
#ifdef _WIN32
typedef struct _stat64 vf_stat_t;
static int vf_stat(const char *path, vf_stat_t *st) {
    VtWBuf w;
    if (!vt_wbuf_from(&w, path)) { vt_wbuf_free(&w); return -1; }
    int r = _wstat64(w.p, st);
    vt_wbuf_free(&w);
    return r;
}
#define VF_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#else
typedef struct stat vf_stat_t;
static int vf_stat(const char *path, vf_stat_t *st) { return stat(path, st); }
#define VF_ISREG(m) S_ISREG(m)
#endif

void *vfile_open(const char *path, const char *mode) {
    return (void *)vt_fopen(path, mode);
}
int64_t vfile_read(void *f, char *buf, int64_t cap) {
    if (!f || cap <= 0) return 0;
    return (int64_t)fread(buf, 1, (size_t)cap, (FILE *)f);
}
/* Read one line (up to and including '\n', or to EOF) into buf/cap via stdio's
   buffered fgets — the streaming counterpart to reading the whole file. Returns
   the byte count (0 at EOF with nothing read). A line longer than cap-1 comes
   back in cap-1 chunks across successive calls (the '\n' arrives on the final
   chunk), so the caller stitches them. Text-oriented: strlen-based, so a line
   with an embedded NUL is reported short. */
int64_t vfile_readline(void *f, char *buf, int64_t cap) {
    if (!f || cap <= 1) return 0;
    if (!fgets(buf, (int)cap, (FILE *)f)) return 0;
    return (int64_t)strlen(buf);
}
int64_t vfile_write(void *f, const char *buf, int64_t n) {
    if (!f || n < 0) return -1;
    return (int64_t)fwrite(buf, 1, (size_t)n, (FILE *)f);
}
/* fseek/ftell take a C long, 32-bit on Windows, so a file past 2 GB could
   not be positioned there. _fseeki64 is in Windows 7's msvcrt; _ftelli64 is
   not, but fgetpos is, and msvcrt's fpos_t is a plain 64-bit offset. */
int vfile_seek(void *f, int64_t off, int whence) {
#ifdef _WIN32
    return f ? _fseeki64((FILE *)f, off, whence) : -1;
#else
    return f ? fseeko((FILE *)f, (off_t)off, whence) : -1;
#endif
}
int64_t vfile_tell(void *f) {
    if (!f) return -1;
#ifdef _WIN32
    fpos_t p;
    if (fgetpos((FILE *)f, &p) != 0) return -1;
    return (int64_t)p;
#else
    return (int64_t)ftello((FILE *)f);
#endif
}
int vfile_eof(void *f) { return f ? (feof((FILE *)f) ? 1 : 0) : 1; }
int vfile_flush(void *f) { return f ? fflush((FILE *)f) : -1; }
void vfile_close(void *f) { if (f) fclose((FILE *)f); }

int64_t vfile_size(const char *path) {
    vf_stat_t st;
    if (vf_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}
int64_t vfile_mtime(const char *path) {
    vf_stat_t st;
    if (vf_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}
int vfile_exists(const char *path) {
    vf_stat_t st;
    return vf_stat(path, &st) == 0 ? 1 : 0;
}
int vfile_is_file(const char *path) {
    vf_stat_t st;
    if (vf_stat(path, &st) != 0) return 0;
    return VF_ISREG(st.st_mode) ? 1 : 0;
}
/* Can this directory actually be listed?

   `isdir` and `file_exists` both answer yes for a directory the process cannot
   open — /root for an unprivileged user is the standard case — and the
   `listdir` builtin PANICS (exit 101) rather than returning empty. That leaves
   no in-language way to check first, which is fatal for anything walking
   user-chosen paths: one step into an unreadable directory kills the process.

   Opening it is the only honest test. access(R_OK) would lie under ACLs and
   would race the open that follows; this performs the same syscall the
   listing will. */
int vfile_dir_readable(const char *path) {
#ifdef _WIN32
    /* No opendir in the MSVC runtime. A directory listing needs the search
       permission that FindFirstFile exercises, so probe with that. */
    WIN32_FIND_DATAW fd;
    HANDLE h;
    VtWBuf w;
    size_t n;
    char *pat;
    if (!path) return 0;
    n = strlen(path);
    pat = (char *)malloc(n + 3);
    if (!pat) return 0;
    memcpy(pat, path, n);
    memcpy(pat + n, "\\*", 3);
    vt_wbuf_from(&w, pat);
    free(pat);
    h = FindFirstFileW(w.p, &fd);
    vt_wbuf_free(&w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
#else
    DIR *d;
    if (!path) return 0;
    d = opendir(path);
    if (!d) return 0;
    closedir(d);
    return 1;
#endif
}

#ifdef _WIN32
#include <direct.h>
int vfile_remove(const char *path) {
    VtWBuf w;
    vt_wbuf_from(&w, path);
    int r = _wremove(w.p);
    vt_wbuf_free(&w);
    return r;
}
/* POSIX rename replaces an existing target; msvcrt's rename refuses. Match
   POSIX, so a write-temp-then-rename save works the same on both. */
int vfile_rename(const char *a, const char *b) {
    VtWBuf wa, wb;
    vt_wbuf_from(&wa, a);
    vt_wbuf_from(&wb, b);
    BOOL ok = MoveFileExW(wa.p, wb.p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    vt_wbuf_free(&wa);
    vt_wbuf_free(&wb);
    return ok ? 0 : -1;
}
/* The MSVC runtime's mkdir takes no mode — Windows has no POSIX permission
   bits to apply, so the directory inherits the parent's ACL either way. */
int vfile_mkdir(const char *path) {
    VtWBuf w;
    vt_wbuf_from(&w, path);
    int r = _wmkdir(w.p);
    vt_wbuf_free(&w);
    return r;
}
#else
int vfile_remove(const char *path) { return remove(path); }
int vfile_rename(const char *a, const char *b) { return rename(a, b); }
int vfile_mkdir(const char *path) { return mkdir(path, 0777); }
#endif
