/* vyto/io/file native backing — thin wrappers over stdio + stat.

   Every buffer transfer is bounded by a length the Vyto side supplies (a
   `bytes(n)` array's capacity), and the actual byte count is returned so Vyto
   never reads past what was filled. The FILE* is an opaque `rawptr` owned by a
   Vyto `File` whose `deinit` calls vfile_close exactly once. */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

void *vfile_open(const char *path, const char *mode) {
    return (void *)fopen(path, mode);
}
long vfile_read(void *f, char *buf, long cap) {
    if (!f || cap <= 0) return 0;
    return (long)fread(buf, 1, (size_t)cap, (FILE *)f);
}
/* Read one line (up to and including '\n', or to EOF) into buf/cap via stdio's
   buffered fgets — the streaming counterpart to reading the whole file. Returns
   the byte count (0 at EOF with nothing read). A line longer than cap-1 comes
   back in cap-1 chunks across successive calls (the '\n' arrives on the final
   chunk), so the caller stitches them. Text-oriented: strlen-based, so a line
   with an embedded NUL is reported short. */
long vfile_readline(void *f, char *buf, long cap) {
    if (!f || cap <= 1) return 0;
    if (!fgets(buf, (int)cap, (FILE *)f)) return 0;
    return (long)strlen(buf);
}
long vfile_write(void *f, const char *buf, long n) {
    if (!f || n < 0) return -1;
    return (long)fwrite(buf, 1, (size_t)n, (FILE *)f);
}
int vfile_seek(void *f, long off, int whence) {
    return f ? fseek((FILE *)f, off, whence) : -1;
}
long vfile_tell(void *f) { return f ? ftell((FILE *)f) : -1; }
int vfile_eof(void *f) { return f ? (feof((FILE *)f) ? 1 : 0) : 1; }
int vfile_flush(void *f) { return f ? fflush((FILE *)f) : -1; }
void vfile_close(void *f) { if (f) fclose((FILE *)f); }

long vfile_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}
long vfile_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_mtime;
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
int vfile_remove(const char *path) { return remove(path); }
int vfile_rename(const char *a, const char *b) { return rename(a, b); }
int vfile_mkdir(const char *path) { return mkdir(path, 0777); }
