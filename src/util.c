#include "util.h"
#include <stdarg.h>

Arena g_arena;

static ArenaChunk *chunk_new(size_t min) {
    size_t cap = 1 << 20;
    if (cap < min) cap = min;
    ArenaChunk *c = malloc(sizeof(ArenaChunk) + cap);
    if (!c) { fprintf(stderr, "vytoc: out of memory\n"); exit(1); }
    c->next = NULL;
    c->used = 0;
    c->cap = cap;
    return c;
}

void *arena_alloc(Arena *a, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!a->head || a->head->used + size > a->head->cap) {
        ArenaChunk *c = chunk_new(size);
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += size;
    memset(p, 0, size);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char tmp[4096];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) {
        va_end(ap2);
        return arena_strndup(a, tmp, (size_t)n);
    }
    char *p = arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

/* ---- interning: simple open hash table ---- */

typedef struct InternEntry {
    const char *str;
    size_t len;
    struct InternEntry *next;
} InternEntry;

#define INTERN_BUCKETS 4096
static InternEntry *intern_tab[INTERN_BUCKETS];

uint64_t fnv1a(const void *data, size_t len) {
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

const char *intern_n(const char *s, size_t n) {
    uint64_t h = fnv1a(s, n) % INTERN_BUCKETS;
    for (InternEntry *e = intern_tab[h]; e; e = e->next)
        if (e->len == n && memcmp(e->str, s, n) == 0) return e->str;
    InternEntry *e = NEW(InternEntry);
    e->str = arena_strndup(&g_arena, s, n);
    e->len = n;
    e->next = intern_tab[h];
    intern_tab[h] = e;
    return e->str;
}

const char *intern(const char *s) { return intern_n(s, strlen(s)); }

/* ---- string buffer ---- */

void sb_init(SBuf *sb) {
    sb->cap = 4096;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    sb->data[0] = 0;
}

static void sb_grow(SBuf *sb, size_t need) {
    if (sb->len + need + 1 <= sb->cap) return;
    while (sb->cap < sb->len + need + 1) sb->cap *= 2;
    sb->data = realloc(sb->data, sb->cap);
}

void sb_putc(SBuf *sb, char c) {
    sb_grow(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = 0;
}

void sb_puts(SBuf *sb, const char *s) {
    size_t n = strlen(s);
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void sb_printf(SBuf *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char tmp[8192];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_grow(sb, (size_t)n);
    if (n < (int)sizeof tmp)
        memcpy(sb->data + sb->len, tmp, (size_t)n);
    else
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
    sb->data[sb->len] = 0;
}

void sb_free(SBuf *sb) { free(sb->data); }

/* ---- diagnostics ---- */

void fatal_at(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", loc.file ? loc.file : "?", loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "vytoc: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---- file reading ---- */

char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = arena_alloc(&g_arena, (size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return NULL; }
    fclose(f);
    buf[sz] = 0;
    if (out_len) *out_len = (size_t)sz;
    return buf;
}
