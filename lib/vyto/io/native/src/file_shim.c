/* vyto/io/file native backing — thin wrappers over stdio + stat.

   Every buffer transfer is bounded by a length the Vyto side supplies (a
   `bytes(n)` array's capacity), and the actual byte count is returned so Vyto
   never reads past what was filled. The FILE* is an opaque `rawptr` owned by a
   Vyto `File` whose `deinit` calls vfile_close exactly once. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

void *vfile_open(const char *path, const char *mode) {
    return (void *)fopen(path, mode);
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
int vfile_seek(void *f, int64_t off, int whence) {
    return f ? fseek((FILE *)f, off, whence) : -1;
}
int64_t vfile_tell(void *f) { return f ? ftell((FILE *)f) : -1; }
int vfile_eof(void *f) { return f ? (feof((FILE *)f) ? 1 : 0) : 1; }
int vfile_flush(void *f) { return f ? fflush((FILE *)f) : -1; }
void vfile_close(void *f) { if (f) fclose((FILE *)f); }

int64_t vfile_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}
int64_t vfile_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}
int vfile_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}
int vfile_is_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
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
    char pat[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!path) return 0;
    _snprintf(pat, sizeof pat, "%s\\*", path);
    pat[sizeof pat - 1] = '\0';
    h = FindFirstFileA(pat, &fd);
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

int vfile_remove(const char *path) { return remove(path); }
int vfile_rename(const char *a, const char *b) { return rename(a, b); }
/* The MSVC runtime's mkdir takes no mode — Windows has no POSIX permission
   bits to apply, so the directory inherits the parent's ACL either way. */
#ifdef _WIN32
#include <direct.h>
int vfile_mkdir(const char *path) { return _mkdir(path); }
#else
int vfile_mkdir(const char *path) { return mkdir(path, 0777); }
#endif
