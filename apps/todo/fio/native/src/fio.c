#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *fio_open_read(const char *path) { return fopen(path, "r"); }
void *fio_open_write(const char *path) { return fopen(path, "w"); }

const char *fio_line(void *f) {
    static char buf[4096];
    if (!f || !fgets(buf, sizeof buf, (FILE *)f)) return 0;
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return buf;
}

void fio_write_line(void *f, const char *s) {
    if (!f) return;
    fputs(s, (FILE *)f);
    fputc('\n', (FILE *)f);
}

void fio_close(void *f) {
    if (f) fclose((FILE *)f);
}

const char *fio_getenv(const char *name) { return getenv(name); }
