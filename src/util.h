#ifndef VYTO_UTIL_H
#define VYTO_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- arena allocator: everything the compiler allocates lives until exit ---- */

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used, cap;
    char data[];
} ArenaChunk;

typedef struct Arena {
    ArenaChunk *head;
} Arena;

void *arena_alloc(Arena *a, size_t size);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_strdup(Arena *a, const char *s);
char *arena_printf(Arena *a, const char *fmt, ...);

extern Arena g_arena; /* single global arena for the whole compile */

#define NEW(T) ((T *)arena_alloc(&g_arena, sizeof(T)))

/* ---- string interning ---- */

const char *intern(const char *s);
const char *intern_n(const char *s, size_t n);

/* ---- growable string buffer (for C emission) ---- */

typedef struct SBuf {
    char *data;
    size_t len, cap;
} SBuf;

void sb_init(SBuf *sb);
void sb_putc(SBuf *sb, char c);
void sb_puts(SBuf *sb, const char *s);
void sb_printf(SBuf *sb, const char *fmt, ...);
void sb_free(SBuf *sb);

/* ---- source locations & diagnostics ---- */

typedef struct Loc {
    const char *file; /* interned path */
    int line, col;
} Loc;

/* print "file:line:col: error: ..." and exit(1) */
#if defined(__GNUC__)
#define VT_NORETURN __attribute__((noreturn))
#else
#define VT_NORETURN
#endif
VT_NORETURN void fatal_at(Loc loc, const char *fmt, ...);
VT_NORETURN void fatal(const char *fmt, ...);

/* ---- misc ---- */

char *read_file(const char *path, size_t *out_len); /* arena-allocated, NUL-terminated; NULL if missing */
uint64_t fnv1a(const void *data, size_t len);

#endif
