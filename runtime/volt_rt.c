#include "volt_rt.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ---- core RC ---- */

void *vt_alloc(size_t size, const VtType *type) {
    VtObj *o = calloc(1, size);
    if (!o) { fprintf(stderr, "volt: out of memory\n"); exit(101); }
    o->rc = 1;
    o->type = type;
    return o;
}

void vt_release(void *p) {
    if (!p) return;
    VtObj *o = (VtObj *)p;
    if (o->rc < 0) return; /* immortal */
    if (--o->rc == 0) {
        if (o->type && o->type->deinit) o->type->deinit(o);
        free(o);
    }
}

void vt_free_now(void *p) { free(p); }

bool vt_isa(const void *p, const VtType *t) {
    if (!p) return false;
    for (const VtType *ty = ((const VtObj *)p)->type; ty; ty = ty->parent)
        if (ty == t) return true;
    return false;
}

void *vt_checked_cast(void *p, const VtType *t, const char *file, int line) {
    if (!p) return NULL;
    if (!vt_isa(p, t)) {
        fprintf(stderr, "%s:%d: panic: invalid cast from %s to %s\n", file, line,
                ((VtObj *)p)->type->name, t->name);
        exit(101);
    }
    return p;
}

void vt_panic_c(const char *file, int line, const char *msg) {
    fprintf(stderr, "%s:%d: panic: %s\n", file, line, msg);
    exit(101);
}

void vt_panic(const char *file, int line, VtString *msg) {
    vt_panic_c(file, line, msg ? msg->data : "(null)");
}

/* ---- strings ---- */

static void str_deinit(void *self) { (void)self; }
static const VtType vt_string_type = {"string", str_deinit, NULL, NULL};

static VtString *str_alloc(int64_t len) {
    VtString *s = malloc(sizeof(VtString) + (size_t)len + 1);
    if (!s) { fprintf(stderr, "volt: out of memory\n"); exit(101); }
    s->hdr.rc = 1;
    s->hdr.type = &vt_string_type;
    s->len = len;
    s->data[len] = 0;
    return s;
}

VtString *vt_str_new(const char *bytes, int64_t len) {
    VtString *s = str_alloc(len);
    memcpy(s->data, bytes, (size_t)len);
    return s;
}

VtString *vt_str_immortal(const char *bytes, int64_t len) {
    VtString *s = vt_str_new(bytes, len);
    s->hdr.rc = -1;
    return s;
}

VtString *vt_str_concat(VtString *a, VtString *b) {
    int64_t la = a ? a->len : 0, lb = b ? b->len : 0;
    VtString *s = str_alloc(la + lb);
    if (a) memcpy(s->data, a->data, (size_t)la);
    if (b) memcpy(s->data + la, b->data, (size_t)lb);
    return s;
}

bool vt_str_eq(VtString *a, VtString *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

VtString *vt_str_from_int(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    return vt_str_new(buf, n);
}

VtString *vt_str_from_float(double v) {
    char buf[40];
    int n = snprintf(buf, sizeof buf, "%g", v);
    return vt_str_new(buf, n);
}

VtString *vt_str_from_bool(bool v) { return vt_str_new(v ? "true" : "false", v ? 4 : 5); }

VtString *vt_str_from_cstr(const char *p) {
    if (!p) return vt_str_new("", 0);
    return vt_str_new(p, (int64_t)strlen(p));
}

VtString *vt_str_slice(VtString *s, int64_t lo, int64_t hi, const char *file, int line) {
    int64_t len = s ? s->len : 0;
    if (lo < 0 || hi < lo || hi > len) {
        char buf[96];
        snprintf(buf, sizeof buf, "slice [%lld, %lld) out of bounds (len %lld)", (long long)lo,
                 (long long)hi, (long long)len);
        vt_panic_c(file, line, buf);
    }
    return vt_str_new(s->data + lo, hi - lo);
}

int64_t vt_str_index(VtString *s, int64_t i, const char *file, int line) {
    if (!s || i < 0 || i >= s->len) vt_panic_c(file, line, "string index out of bounds");
    return (int64_t)(unsigned char)s->data[i];
}

void vt_print(VtString *s) {
    if (s) fwrite(s->data, 1, (size_t)s->len, stdout);
    fputc('\n', stdout);
}

/* ---- file I/O ---- */

static FILE *file_open(VtString *path, const char *mode, const char *file, int line) {
    FILE *f = fopen(vt_str_cstr(path), mode);
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof buf, "cannot open file: %s", vt_str_cstr(path));
        vt_panic_c(file, line, buf);
    }
    return f;
}

VtString *vt_file_read(VtString *path, const char *file, int line) {
    FILE *f = file_open(path, "rb", file, line);
    if (fseek(f, 0, SEEK_END) != 0) vt_panic_c(file, line, "cannot seek in file");
    long n = ftell(f);
    if (n < 0) vt_panic_c(file, line, "cannot read file size");
    fseek(f, 0, SEEK_SET);
    VtString *s = str_alloc(n);
    if (fread(s->data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        vt_panic_c(file, line, "short read");
    }
    fclose(f);
    return s;
}

bool vt_file_write(VtString *path, VtString *data, bool append) {
    FILE *f = fopen(vt_str_cstr(path), append ? "ab" : "wb");
    if (!f) return false;
    size_t n = data ? (size_t)data->len : 0;
    bool ok = fwrite(data ? data->data : "", 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

VtArray *vt_file_lines(VtString *path, const char *file, int line) {
    VtString *whole = vt_file_read(path, file, line);
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    const char *p = whole->data, *end = whole->data + whole->len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *stop = nl ? nl : end;
        size_t l = (size_t)(stop - p);
        if (l > 0 && stop[-1] == '\r') l--; /* CRLF */
        VtString *s = vt_str_new(p, (int64_t)l);
        vt_arr_push(a, &s); /* push retains */
        vt_release(s);
        if (!nl) break;
        p = nl + 1;
    }
    vt_release(whole);
    return a;
}

static int cstr_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* directory entry names (excluding ".", including ".."), sorted ascending */
VtArray *vt_dir_list(VtString *path, const char *file, int line) {
    const char *dir = vt_str_cstr(path);
    char *names[4096];
    int n = 0;
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        char buf[512];
        snprintf(buf, sizeof buf, "cannot open directory: %s", dir);
        vt_panic_c(file, line, buf);
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0) continue;
        if (n < 4096) {
            size_t len = strlen(fd.cFileName) + 1;
            names[n] = malloc(len);
            memcpy(names[n], fd.cFileName, len);
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) {
        char buf[512];
        snprintf(buf, sizeof buf, "cannot open directory: %s", dir);
        vt_panic_c(file, line, buf);
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0) continue;
        if (n < 4096) {
            size_t len = strlen(de->d_name) + 1;
            names[n] = malloc(len);
            memcpy(names[n], de->d_name, len);
            n++;
        }
    }
    closedir(d);
#endif
    qsort(names, (size_t)n, sizeof names[0], cstr_cmp);
    VtArray *a = vt_arr_new(sizeof(VtString *), true);
    for (int i = 0; i < n; i++) {
        VtString *s = vt_str_from_cstr(names[i]);
        vt_arr_push(a, &s); /* push retains */
        vt_release(s);
        free(names[i]);
    }
    return a;
}

bool vt_is_dir(VtString *path) {
    const char *p = vt_str_cstr(path);
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* ---- arrays ---- */

static void arr_deinit(void *self) {
    VtArray *a = self;
    if (a->elem_ref)
        for (int64_t i = 0; i < a->len; i++)
            vt_release(*(void **)(a->data + i * a->elem_size));
    free(a->data);
}
static const VtType vt_array_type = {"array", arr_deinit, NULL, NULL};

VtArray *vt_arr_new(int32_t elem_size, bool elem_ref) {
    VtArray *a = vt_alloc(sizeof(VtArray), &vt_array_type);
    a->elem_size = elem_size;
    a->elem_ref = elem_ref;
    return a;
}

VtArray *vt_arr_bytes(int64_t n) {
    VtArray *a = vt_arr_new(1, false);
    if (n < 0) n = 0;
    a->data = calloc((size_t)n ? (size_t)n : 1, 1);
    if (!a->data) { fprintf(stderr, "volt: out of memory\n"); exit(101); }
    a->len = a->cap = n;
    return a;
}

void vt_arr_push(VtArray *a, const void *elem) {
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->data = realloc(a->data, (size_t)(a->cap * a->elem_size));
        if (!a->data) { fprintf(stderr, "volt: out of memory\n"); exit(101); }
    }
    memcpy(a->data + a->len * a->elem_size, elem, (size_t)a->elem_size);
    if (a->elem_ref) vt_retain(*(void **)elem);
    a->len++;
}

void vt_arr_pop(VtArray *a, void *out, const char *file, int line) {
    if (a->len == 0) vt_panic_c(file, line, "pop from empty array");
    a->len--;
    memcpy(out, a->data + a->len * a->elem_size, (size_t)a->elem_size);
    /* ownership transfers to caller: no release, no retain */
}

void *vt_arr_at(VtArray *a, int64_t i, const char *file, int line) {
    if (!a) vt_panic_c(file, line, "index into null array");
    if (i < 0 || i >= a->len) {
        char buf[96];
        snprintf(buf, sizeof buf, "array index %lld out of bounds (len %lld)",
                 (long long)i, (long long)a->len);
        vt_panic_c(file, line, buf);
    }
    return a->data + i * a->elem_size;
}

void vt_arr_set(VtArray *a, int64_t i, const void *elem, const char *file, int line) {
    void *slot = vt_arr_at(a, i, file, line);
    if (a->elem_ref) {
        vt_retain(*(void **)elem);
        vt_release(*(void **)slot);
    }
    memcpy(slot, elem, (size_t)a->elem_size);
}

/* ---- maps ---- */

static void map_deinit(void *self) {
    VtMap *m = self;
    for (int64_t b = 0; b < m->nbuckets; b++) {
        VtMapEntry *e = m->buckets[b];
        while (e) {
            VtMapEntry *next = e->next;
            vt_release(e->key);
            if (m->val_ref) vt_release((void *)(uintptr_t)e->val);
            free(e);
            e = next;
        }
    }
    free(m->buckets);
}
static const VtType vt_map_type = {"map", map_deinit, NULL, NULL};

VtMap *vt_map_new(bool val_ref) {
    VtMap *m = vt_alloc(sizeof(VtMap), &vt_map_type);
    m->nbuckets = 64;
    m->buckets = calloc((size_t)m->nbuckets, sizeof(VtMapEntry *));
    m->val_ref = val_ref;
    return m;
}

static uint64_t map_hash(VtString *k) {
    uint64_t h = 1469598103934665603ull;
    for (int64_t i = 0; i < k->len; i++) { h ^= (unsigned char)k->data[i]; h *= 1099511628211ull; }
    return h;
}

static VtMapEntry **map_slot(VtMap *m, VtString *key) {
    VtMapEntry **pe = &m->buckets[map_hash(key) % (uint64_t)m->nbuckets];
    while (*pe && !vt_str_eq((*pe)->key, key)) pe = &(*pe)->next;
    return pe;
}

void vt_map_set(VtMap *m, VtString *key, uint64_t val) {
    VtMapEntry **pe = map_slot(m, key);
    if (*pe) {
        if (m->val_ref) {
            vt_retain((void *)(uintptr_t)val);
            vt_release((void *)(uintptr_t)(*pe)->val);
        }
        (*pe)->val = val;
        return;
    }
    VtMapEntry *e = malloc(sizeof *e);
    if (!e) { fprintf(stderr, "volt: out of memory\n"); exit(101); }
    vt_retain(key);
    e->key = key;
    e->val = val;
    e->next = NULL;
    if (m->val_ref) vt_retain((void *)(uintptr_t)val);
    *pe = e;
    m->len++;
}

uint64_t vt_map_get(VtMap *m, VtString *key, const char *file, int line) {
    VtMapEntry **pe = map_slot(m, key);
    if (!*pe) {
        char buf[160];
        snprintf(buf, sizeof buf, "map key not found: %s", key ? key->data : "(null)");
        vt_panic_c(file, line, buf);
    }
    return (*pe)->val;
}

bool vt_map_has(VtMap *m, VtString *key) { return *map_slot(m, key) != NULL; }

void vt_map_remove(VtMap *m, VtString *key) {
    VtMapEntry **pe = map_slot(m, key);
    if (!*pe) return;
    VtMapEntry *e = *pe;
    *pe = e->next;
    vt_release(e->key);
    if (m->val_ref) vt_release((void *)(uintptr_t)e->val);
    free(e);
    m->len--;
}

/* ---- closures ---- */

static void closure_deinit(void *self) {
    VtClosure *c = self;
    vt_release(c->env);
}
static const VtType vt_closure_type = {"closure", closure_deinit, NULL, NULL};

VtClosure *vt_closure_new(void *fn, VtObj *env) {
    VtClosure *c = vt_alloc(sizeof(VtClosure), &vt_closure_type);
    c->fn = fn;
    c->env = env; /* ownership transferred */
    return c;
}
