/* vyto/data native backing — a columnar table engine (the ct_* API).

   Struct-of-arrays: every column is one contiguous typed buffer (i64 / f64 /
   bool / string-offset-into-a-shared-arena), so a cell is a single indexed load
   with zero per-cell boxing. Sort and filter never move column data — they only
   reorder / compact an `idx` permutation that names the currently-visible rows.
   That makes a 1M-row sort a permutation of one int64 array, and a filter a
   single compacting pass, both O(n) memory and comfortably sub-second.

   The store is an opaque rawptr owned by a Vyto DataFrame whose deinit calls
   ct_free exactly once — the same opaque-handle + explicit-free shape as
   sb_shim.c / file_shim.c. All buffers double on growth; ct_reserve pre-sizes so
   a bulk load is realloc-free.

   Memory contract (dense): bytes ~= nrow * (sum of column widths) + 8*nrow for
   idx. i64/f64 col = 8B/row, bool = 1B/row, string = 8B(soff)+4B(slen)/row plus
   the arena bytes. Thousands of dense columns scale as nrow*ncol*8 — the caller
   sizes accordingly. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>

enum { CT_I64 = 0, CT_F64 = 1, CT_BOOL = 2, CT_STR = 3, CT_CAT = 4,
       CT_I32 = 5, CT_I16 = 6, CT_F32 = 7, CT_AUTO = 8 };

/* A promotable (CK_AUTO) column starts as CT_I16 and widens itself as values
   arrive; promote once its dictionary grows past this many distinct values
   (categorical then costs more than raw strings). */
#define CT_CAT_MAX_DICT 65536

/* one column.
   - scalar kinds: `data` is int64 / double / uint8, one per row.
   - CT_STR: soff/slen give each row's byte span in the shared `arena`.
   - CT_CAT: `data` is an int32 dictionary code per row; the dictionary's unique
     strings live in `arena` (doff/dlen span each entry), interned through the
     open-addressed `dhash`. Low-cardinality text (regions, categories, repeated
     labels) costs 4 bytes/row plus one copy of each distinct value, instead of a
     full string per row. */
typedef struct {
    int kind;
    char *name;
    void *data;        /* scalar kinds; CT_CAT: int32 code per row */
    int64_t *soff;     /* CT_STR: start offset of each row's bytes in arena */
    int32_t *slen;     /* CT_STR: byte length of each row's string */
    char *arena;       /* CT_STR rows / CT_CAT dictionary: packed bytes */
    size_t alen, acap; /* arena used / capacity */
    /* CT_CAT dictionary */
    int64_t *doff;     /* offset of each dict entry in arena */
    int32_t *dlen;     /* byte length of each dict entry */
    int ndict, dcap;   /* live dict entries / capacity */
    int32_t *dhash;    /* open-addressed intern index (entry+1; 0 = empty) */
    int dhmask;        /* dhash size - 1 (power of two), 0 = unallocated */
    uint8_t promotable; /* CK_AUTO: self-widen `kind` as values arrive */
} Col;

typedef struct {
    Col *cols;
    int ncol, ccap;    /* column count / column-array capacity */
    int64_t nrow, cap; /* row count / row capacity of every column */
    int32_t *idx;      /* visible permutation (4B/row); rows capped at 2^31 */
    int64_t vlen;      /* number of visible rows (after filter) */
} CT;

/* ------------------------------------------------------------- allocation */

static size_t col_elem(int kind) {
    if (kind == CT_F64) return sizeof(double);
    if (kind == CT_F32) return sizeof(float);
    if (kind == CT_BOOL) return 1;
    if (kind == CT_I16) return sizeof(int16_t);
    if (kind == CT_I32 || kind == CT_CAT) return sizeof(int32_t); /* i32, or dict code */
    return sizeof(int64_t); /* CT_I64 */
}

static void scalar_put_i(Col *c, int64_t r, int64_t v);  /* defined below */

/* ------------------------------------------------- categorical dictionary */

/* FNV-1a over the raw bytes. */
static uint32_t cat_h(const char *s, int32_t n) {
    uint32_t h = 2166136261u;
    for (int32_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* Grow (or first-build) the intern hash and re-insert every existing entry. */
static int cat_rehash(Col *c, int newmask) {
    int32_t *nh = (int32_t *)calloc((size_t)newmask + 1, sizeof(int32_t));
    if (!nh) return 0;
    for (int e = 0; e < c->ndict; e++) {
        uint32_t h = cat_h(c->arena + c->doff[e], c->dlen[e]) & (uint32_t)newmask;
        while (nh[h]) { h = (h + 1) & (uint32_t)newmask; }
        nh[h] = e + 1;
    }
    free(c->dhash);
    c->dhash = nh;
    c->dhmask = newmask;
    return 1;
}

/* Intern (s,n) into c's dictionary, returning its code (index). -1 on OOM. */
static int cat_intern(Col *c, const char *s, int32_t n) {
    if (c->dhmask == 0 || c->ndict * 2 >= c->dhmask) {
        int nm = c->dhmask ? c->dhmask * 2 + 1 : 15;
        if (!cat_rehash(c, nm)) return -1;
    }
    uint32_t h = cat_h(s, n) & (uint32_t)c->dhmask;
    while (c->dhash[h]) {
        int e = c->dhash[h] - 1;
        if (c->dlen[e] == n && (n == 0 || memcmp(c->arena + c->doff[e], s, (size_t)n) == 0)) return e;
        h = (h + 1) & (uint32_t)c->dhmask;
    }
    /* miss → append a new dict entry */
    if (c->ndict == c->dcap) {
        int nc = c->dcap ? c->dcap * 2 : 16;
        int64_t *no = (int64_t *)realloc(c->doff, (size_t)nc * sizeof(int64_t));
        int32_t *nl = (int32_t *)realloc(c->dlen, (size_t)nc * sizeof(int32_t));
        if (!no || !nl) return -1;
        c->doff = no; c->dlen = nl; c->dcap = nc;
    }
    if (c->alen + (size_t)n > c->acap) {
        size_t na = c->acap ? c->acap : 64;
        while (na < c->alen + (size_t)n) na *= 2;
        char *nb = (char *)realloc(c->arena, na);
        if (!nb) return -1;
        c->arena = nb; c->acap = na;
    }
    int e = c->ndict++;
    c->doff[e] = (int64_t)c->alen;
    c->dlen[e] = n;
    if (n > 0) { memcpy(c->arena + c->alen, s, (size_t)n); c->alen += (size_t)n; }
    c->dhash[h] = e + 1;
    return e;
}

/* Bytes + length of a CT_CAT row's string (via its dict code). */
static const char *cat_str(const Col *c, int64_t row, int32_t *len_out) {
    int32_t code = ((int32_t *)c->data)[row];
    if (code < 0 || code >= c->ndict) { *len_out = 0; return ""; }
    *len_out = c->dlen[code];
    return c->arena + c->doff[code];
}

/* grow every column's row buffers (and idx) to at least `need` rows. */
static int ct_grow(CT *t, int64_t need) {
    if (need <= t->cap) return 1;
    int64_t nc = t->cap ? t->cap : 64;
    while (nc < need) nc *= 2;
    for (int c = 0; c < t->ncol; c++) {
        Col *col = &t->cols[c];
        if (col->kind == CT_STR) {
            int64_t *no = (int64_t *)realloc(col->soff, (size_t)nc * sizeof(int64_t));
            int32_t *nl = (int32_t *)realloc(col->slen, (size_t)nc * sizeof(int32_t));
            if (!no || !nl) return 0;
            col->soff = no;
            col->slen = nl;
        } else {
            void *nd = realloc(col->data, (size_t)nc * col_elem(col->kind));
            if (!nd) return 0;
            col->data = nd;
        }
    }
    int32_t *ni = (int32_t *)realloc(t->idx, (size_t)nc * sizeof(int32_t));
    if (!ni) return 0;
    t->idx = ni;
    t->cap = nc;
    return 1;
}

void *ct_new(long ncap) {
    CT *t = (CT *)calloc(1, sizeof *t);
    if (!t) return NULL;
    if (ncap > 0 && !ct_grow(t, ncap)) { free(t); return NULL; }
    return t;
}

/* pre-size all columns + idx to `nrow` rows (one realloc each, no-op if smaller
   than current capacity). Call after the schema is set and the row count is
   known so the bulk append below never reallocs. */
void ct_reserve(void *h, long nrow) {
    if (h && nrow > 0) ct_grow((CT *)h, (int64_t)nrow);
}

int ct_add_col(void *h, const char *name, int kind) {
    CT *t = (CT *)h;
    if (!t) return -1;
    if (t->ncol == t->ccap) {
        int ncc = t->ccap ? t->ccap * 2 : 8;
        Col *nc = (Col *)realloc(t->cols, (size_t)ncc * sizeof(Col));
        if (!nc) return -1;
        t->cols = nc;
        t->ccap = ncc;
    }
    Col *col = &t->cols[t->ncol];
    memset(col, 0, sizeof *col);
    if (kind == CT_AUTO) { col->promotable = 1; kind = CT_I16; } /* start narrow, self-widen */
    col->kind = kind;
    col->name = name ? strdup(name) : strdup("");
    /* size the new column to the current row capacity so it lines up with the
       columns already present */
    if (t->cap > 0) {
        if (kind == CT_STR) {
            col->soff = (int64_t *)malloc((size_t)t->cap * sizeof(int64_t));
            col->slen = (int32_t *)malloc((size_t)t->cap * sizeof(int32_t));
        } else {
            col->data = malloc((size_t)t->cap * col_elem(kind));
        }
    }
    if (kind == CT_CAT) { cat_intern(col, "", 0); } /* code 0 = "" default */
    /* back-fill defaults for any rows that already exist */
    for (int64_t r = 0; r < t->nrow; r++) {
        if (kind == CT_STR) { col->soff[r] = (int64_t)col->alen; col->slen[r] = 0; }
        else if (kind == CT_CAT) ((int32_t *)col->data)[r] = 0;
        else scalar_put_i(col, r, 0);
    }
    return t->ncol++;
}

/* ------------------------------------------------------------- row build */

/* Open a new row: ensure capacity and default every column at slot `nrow`.
   set_* overwrite those defaults; end_row commits. */
void ct_begin_row(void *h) {
    CT *t = (CT *)h;
    if (!t) return;
    if (!ct_grow(t, t->nrow + 1)) return;
    int64_t r = t->nrow;
    for (int c = 0; c < t->ncol; c++) {
        Col *col = &t->cols[c];
        if (col->kind == CT_STR) { col->soff[r] = (int64_t)col->alen; col->slen[r] = 0; }
        else if (col->kind == CT_CAT) ((int32_t *)col->data)[r] = 0;
        else scalar_put_i(col, r, 0);
    }
}

static double cell_num(const Col *c, int64_t row);  /* defined below */
static int kind_is_int(int k);

/* Store an integer into a scalar column at row `r` (exact for the integer
   kinds — no double round-trip; smaller kinds wrap on overflow like a C cast). */
static void scalar_put_i(Col *c, int64_t r, int64_t v) {
    if (c->kind == CT_I64) ((int64_t *)c->data)[r] = v;
    else if (c->kind == CT_I32) ((int32_t *)c->data)[r] = (int32_t)v;
    else if (c->kind == CT_I16) ((int16_t *)c->data)[r] = (int16_t)v;
    else if (c->kind == CT_BOOL) ((uint8_t *)c->data)[r] = v != 0 ? 1 : 0;
    else if (c->kind == CT_F64) ((double *)c->data)[r] = (double)v;
    else if (c->kind == CT_F32) ((float *)c->data)[r] = (float)v;
}

/* Store a float into a scalar column at row `r`, truncating for integer kinds. */
static void scalar_put_f(Col *c, int64_t r, double v) {
    if (c->kind == CT_F64) ((double *)c->data)[r] = v;
    else if (c->kind == CT_F32) ((float *)c->data)[r] = (float)v;
    else scalar_put_i(c, r, (int64_t)v);
}

/* Append raw bytes as a CT_STR row. */
static void str_append(Col *c, int64_t row, const char *s, int32_t n) {
    if (c->alen + (size_t)n > c->acap) {
        size_t na = c->acap ? c->acap : 256;
        while (na < c->alen + (size_t)n) na *= 2;
        char *nb = (char *)realloc(c->arena, na);
        if (!nb) return;
        c->arena = nb; c->acap = na;
    }
    c->soff[row] = (int64_t)c->alen;
    c->slen[row] = n;
    if (n > 0) { memcpy(c->arena + c->alen, s, (size_t)n); c->alen += (size_t)n; }
}

/* Store text into a text column (CT_CAT interns, CT_STR appends). */
static void text_store(Col *c, int64_t row, const char *s, int32_t n) {
    if (c->kind == CT_CAT) {
        int code = cat_intern(c, s, n);
        if (code >= 0) ((int32_t *)c->data)[row] = code;
    } else {
        str_append(c, row, s, n);
    }
}

/* ------------------------------------------------- auto (CK_AUTO) promotion */

/* Format a scalar row's value as text (into buf); returns the byte length. */
static int fmt_scalar(const Col *c, int64_t row, char *buf, int cap) {
    if (kind_is_int(c->kind)) return snprintf(buf, (size_t)cap, "%lld", (long long)cell_num(c, row));
    return snprintf(buf, (size_t)cap, "%g", cell_num(c, row));
}

/* Rewrite a scalar column to a wider scalar `newkind`, converting each existing
   row. Cheap because it fires at most a handful of times over a whole load. */
static void numeric_convert(Col *c, int64_t nrow, int64_t cap, int newkind) {
    void *nb = malloc((size_t)cap * col_elem(newkind));
    if (!nb) return;
    void *old = c->data;
    int oldk = c->kind;
    c->data = nb; c->kind = newkind;
    for (int64_t r = 0; r < nrow; r++) {
        Col tmp = *c; tmp.data = old; tmp.kind = oldk;   /* read through old view */
        double v = cell_num(&tmp, r);
        if (kind_is_int(newkind)) scalar_put_i(c, r, (int64_t)v);
        else scalar_put_f(c, r, v);
    }
    free(old);
}

/* Convert a scalar column to CT_CAT: each existing value becomes its text form,
   interned into a fresh dictionary. */
static void numeric_to_cat(Col *c, int64_t nrow, int64_t cap) {
    int32_t *codes = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
    if (!codes) return;
    void *old = c->data;
    int oldk = c->kind;
    char buf[48];
    for (int64_t r = 0; r < nrow; r++) {
        Col tmp = *c; tmp.data = old; tmp.kind = oldk;
        int n = fmt_scalar(&tmp, r, buf, sizeof buf);
        codes[r] = cat_intern(c, buf, n);   /* builds c->arena/doff/dlen/dhash */
    }
    c->data = codes; c->kind = CT_CAT;
    free(old);
}

/* Convert a CT_CAT column to CT_STR (materialize each row's dictionary string).
   Used when the value count outgrows the dictionary's benefit. */
static void cat_to_str(Col *c, int64_t nrow, int64_t cap) {
    int64_t *soff = (int64_t *)malloc((size_t)cap * sizeof(int64_t));
    int32_t *slen = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
    if (!soff || !slen) { free(soff); free(slen); return; }
    int32_t *codes = (int32_t *)c->data;
    char *dict = c->arena; int64_t *doff = c->doff; int32_t *dlen = c->dlen;
    /* build a fresh arena; keep the dict arena around until we've copied out */
    char *na = NULL; size_t nalen = 0, nacap = 0;
    for (int64_t r = 0; r < nrow; r++) {
        int32_t code = codes[r];
        int32_t l = (code >= 0 && code < c->ndict) ? dlen[code] : 0;
        if (nalen + (size_t)l > nacap) {
            size_t g = nacap ? nacap : 256;
            while (g < nalen + (size_t)l) g *= 2;
            char *t = (char *)realloc(na, g); if (!t) { free(na); free(soff); free(slen); return; }
            na = t; nacap = g;
        }
        soff[r] = (int64_t)nalen;
        slen[r] = l;
        if (l > 0) { memcpy(na + nalen, dict + doff[code], (size_t)l); nalen += (size_t)l; }
    }
    free(c->data); free(c->arena); free(c->doff); free(c->dlen); free(c->dhash);
    c->data = NULL; c->soff = soff; c->slen = slen;
    c->arena = na; c->alen = nalen; c->acap = nacap;
    c->doff = NULL; c->dlen = NULL; c->dhash = NULL; c->ndict = 0; c->dcap = 0; c->dhmask = 0;
    c->kind = CT_STR;
}

static int f32_safe(double v) { return (double)(float)v == v; }

/* smallest signed-int kind that holds v */
static int int_kind_for(int64_t v) {
    if (v >= -32768 && v <= 32767) return CT_I16;
    if (v >= -2147483648LL && v <= 2147483647LL) return CT_I32;
    return CT_I64;
}
static int wider_int(int a, int b) {   /* pick the wider of two int kinds */
    int wa = a == CT_I16 ? 0 : a == CT_I32 ? 1 : 2;
    int wb = b == CT_I16 ? 0 : b == CT_I32 ? 1 : 2;
    int w = wa > wb ? wa : wb;
    return w == 0 ? CT_I16 : w == 1 ? CT_I32 : CT_I64;
}

/* Feed one value into a promotable column at row `row`; `nrow` = committed rows
   (those needing conversion on a widen). */
static void auto_feed_i(Col *c, int64_t row, int64_t nrow, int64_t cap, int64_t v) {
    if (kind_is_int(c->kind)) {
        int need = wider_int(c->kind, int_kind_for(v));
        if (need != c->kind) numeric_convert(c, nrow, cap, need);
        scalar_put_i(c, row, v);
    } else if (c->kind == CT_F32 || c->kind == CT_F64) {
        scalar_put_i(c, row, v);
    } else { /* text */
        char buf[32]; int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
        text_store(c, row, buf, n);
    }
}

static void auto_feed_f(Col *c, int64_t row, int64_t nrow, int64_t cap, double v) {
    if (kind_is_int(c->kind)) {
        numeric_convert(c, nrow, cap, f32_safe(v) ? CT_F32 : CT_F64);
        scalar_put_f(c, row, v);
    } else if (c->kind == CT_F32) {
        if (!f32_safe(v)) numeric_convert(c, nrow, cap, CT_F64);
        scalar_put_f(c, row, v);
    } else if (c->kind == CT_F64) {
        scalar_put_f(c, row, v);
    } else { /* text */
        char buf[32]; int n = snprintf(buf, sizeof buf, "%g", v);
        text_store(c, row, buf, n);
    }
}

/* Classify a field: 0 = int (→*iv), 1 = float (→*dv), 2 = text. */
static int parse_scalar(const char *s, int32_t n, int64_t *iv, double *dv) {
    const char *p = s, *e = s + n;
    while (p < e && isspace((unsigned char)*p)) p++;
    if (p == e) return 2;
    char *end;
    long long i = strtoll(p, &end, 10);
    const char *q = end;
    while (q < e && isspace((unsigned char)*q)) q++;
    if (end != p && q == e) { *iv = (int64_t)i; return 0; }
    double d = strtod(p, &end);
    q = end;
    while (q < e && isspace((unsigned char)*q)) q++;
    if (end != p && q == e) { *dv = d; return 1; }
    return 2;
}

static void auto_feed_str(Col *c, int64_t row, int64_t nrow, int64_t cap, const char *s, int32_t n) {
    if (c->kind == CT_CAT || c->kind == CT_STR) {
        text_store(c, row, s, n);
        if (c->kind == CT_CAT && c->ndict > CT_CAT_MAX_DICT) cat_to_str(c, nrow + 1, cap);
        return;
    }
    /* numeric column: does the text parse as a number? */
    int64_t iv; double dv;
    int k = parse_scalar(s, n, &iv, &dv);
    if (k == 0) { auto_feed_i(c, row, nrow, cap, iv); }
    else if (k == 1) { auto_feed_f(c, row, nrow, cap, dv); }
    else { numeric_to_cat(c, nrow, cap); text_store(c, row, s, n); }  /* non-numeric → categorical */
}

void ct_set_i64(void *h, int col, long long v) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->promotable) auto_feed_i(c, t->nrow, t->nrow, t->cap, (int64_t)v);
    else scalar_put_i(c, t->nrow, (int64_t)v);
}

void ct_set_f64(void *h, int col, double v) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->promotable) auto_feed_f(c, t->nrow, t->nrow, t->cap, v);
    else scalar_put_f(c, t->nrow, v);
}

void ct_set_bool(void *h, int col, int v) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->promotable) auto_feed_i(c, t->nrow, t->nrow, t->cap, v ? 1 : 0);
    else scalar_put_i(c, t->nrow, v ? 1 : 0);
}

void ct_set_str(void *h, int col, const char *s, long n) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (!s || n < 0) return;
    if (c->promotable) { auto_feed_str(c, t->nrow, t->nrow, t->cap, s, (int32_t)n); return; }
    if (c->kind == CT_CAT) {
        int code = cat_intern(c, s, (int32_t)n);
        if (code >= 0) ((int32_t *)c->data)[t->nrow] = code;
        return;
    }
    if (c->kind != CT_STR) return;
    str_append(c, t->nrow, s, (int32_t)n);
}

void ct_end_row(void *h) {
    CT *t = (CT *)h;
    if (!t) return;
    t->idx[t->nrow] = t->nrow;
    t->nrow++;
    t->vlen = t->nrow; /* no filter active while building */
}

/* ------------------------------------------------------------- shape */

long ct_nrow(void *h) { return h ? (long)((CT *)h)->nrow : 0; }
long ct_ncol(void *h) { return h ? (long)((CT *)h)->ncol : 0; }
long ct_view_len(void *h) { return h ? (long)((CT *)h)->vlen : 0; }

int ct_col_kind(void *h, int col) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return -1;
    return t->cols[col].kind;
}

long ct_col_name(void *h, int col, char *buf, long cap) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol || cap <= 1) { if (buf && cap > 0) buf[0] = 0; return 0; }
    const char *nm = t->cols[col].name ? t->cols[col].name : "";
    long n = (long)strlen(nm);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, nm, (size_t)n);
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------- cell read */

static double cell_num(const Col *c, int64_t row) {
    if (c->kind == CT_F64) return ((double *)c->data)[row];
    if (c->kind == CT_F32) return (double)((float *)c->data)[row];
    if (c->kind == CT_BOOL) return ((uint8_t *)c->data)[row] ? 1.0 : 0.0;
    if (c->kind == CT_I64) return (double)((int64_t *)c->data)[row];
    if (c->kind == CT_I32) return (double)((int32_t *)c->data)[row];
    if (c->kind == CT_I16) return (double)((int16_t *)c->data)[row];
    return 0.0;
}

/* True for the integer-valued scalar kinds (cellStr prints them without a
   decimal point). */
static int kind_is_int(int k) { return k == CT_I64 || k == CT_I32 || k == CT_I16; }

/* Format visible row `vrow`, column `col` into buf (NUL-terminated), returning
   the byte length. Scalars via snprintf, strings copied from the arena. This is
   the only text a rendered cell costs — no whole-model string materialization. */
long ct_cell_str(void *h, long vrow, int col, char *buf, long cap) {
    CT *t = (CT *)h;
    if (!t || vrow < 0 || vrow >= t->vlen || col < 0 || col >= t->ncol || cap <= 1) {
        if (buf && cap > 0) buf[0] = 0;
        return 0;
    }
    int64_t row = t->idx[vrow];
    Col *c = &t->cols[col];
    if (c->kind == CT_STR || c->kind == CT_CAT) {
        long n;
        const char *p;
        if (c->kind == CT_CAT) { int32_t l; p = cat_str(c, row, &l); n = l; }
        else { n = c->slen[row]; p = c->arena + c->soff[row]; }
        if (n > cap - 1) n = cap - 1;
        memcpy(buf, p, (size_t)n);
        buf[n] = 0;
        return n;
    }
    if (kind_is_int(c->kind)) return snprintf(buf, (size_t)cap, "%lld", (long long)cell_num(c, row));
    if (c->kind == CT_BOOL) { const char *s = ((uint8_t *)c->data)[row] ? "true" : "false"; long n = (long)strlen(s); memcpy(buf, s, (size_t)n + 1); return n; }
    return snprintf(buf, (size_t)cap, "%g", cell_num(c, row));
}

long long ct_cell_i64(void *h, long vrow, int col) {
    CT *t = (CT *)h;
    if (!t || vrow < 0 || vrow >= t->vlen || col < 0 || col >= t->ncol) return 0;
    return (long long)cell_num(&t->cols[col], t->idx[vrow]);
}

double ct_cell_f64(void *h, long vrow, int col) {
    CT *t = (CT *)h;
    if (!t || vrow < 0 || vrow >= t->vlen || col < 0 || col >= t->ncol) return 0.0;
    return cell_num(&t->cols[col], t->idx[vrow]);
}

/* ------------------------------------------------------------- sort */

/* Compare two absolute rows on one column; returns <0 / 0 / >0. */
static int cmp_col(const Col *c, int64_t a, int64_t b) {
    if (c->kind == CT_STR || c->kind == CT_CAT) {
        const char *pa, *pb;
        int32_t la, lb;
        if (c->kind == CT_CAT) {
            int32_t ca = ((int32_t *)c->data)[a], cb = ((int32_t *)c->data)[b];
            if (ca == cb) return 0;                       /* same code = same string */
            pa = c->arena + c->doff[ca]; la = c->dlen[ca];
            pb = c->arena + c->doff[cb]; lb = c->dlen[cb];
        } else {
            pa = c->arena + c->soff[a]; la = c->slen[a];
            pb = c->arena + c->soff[b]; lb = c->slen[b];
        }
        int32_t m = la < lb ? la : lb;
        int r = m > 0 ? memcmp(pa, pb, (size_t)m) : 0;
        if (r) return r;
        return la - lb;
    }
    double x = cell_num(c, a), y = cell_num(c, b);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

typedef struct { CT *t; const int *cols; const int *dirs; int nk; } SortCtx;

static int cmp_rows(const SortCtx *s, int64_t a, int64_t b) {
    for (int k = 0; k < s->nk; k++) {
        int col = s->cols[k];
        if (col < 0 || col >= s->t->ncol) continue;
        int r = cmp_col(&s->t->cols[col], a, b);
        if (r) return s->dirs[k] < 0 ? -r : r;
    }
    return 0;
}

/* Bottom-up stable merge sort of the visible idx window by the key list.
   Stable so multi-key ties keep their prior order; O(n log n), one temp buffer.
   1M rows sort well under a second. */
static void ct_msort(CT *t, const int *cols, const int *dirs, int nk) {
    int64_t n = t->vlen;
    if (n < 2) return;
    SortCtx s = { t, cols, dirs, nk };
    int32_t *src = t->idx;
    int32_t *tmp = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    if (!tmp) return;
    int32_t *buf = tmp, *cur = src;
    for (int64_t w = 1; w < n; w *= 2) {
        for (int64_t i = 0; i < n; i += 2 * w) {
            int64_t l = i, mid = i + w < n ? i + w : n, r = i + 2 * w < n ? i + 2 * w : n;
            int64_t a = l, b = mid, o = l;
            while (a < mid && b < r) {
                if (cmp_rows(&s, cur[a], cur[b]) <= 0) buf[o++] = cur[a++];
                else buf[o++] = cur[b++];
            }
            while (a < mid) buf[o++] = cur[a++];
            while (b < r) buf[o++] = cur[b++];
        }
        int32_t *sw = cur; cur = buf; buf = sw;
    }
    if (cur != src) { memcpy(src, cur, (size_t)n * sizeof(int32_t)); }
    free(tmp);
}

void ct_sort(void *h, int col, int dir) {
    CT *t = (CT *)h;
    if (!t) return;
    int cols[1] = { col }, dirs[1] = { dir };
    ct_msort(t, cols, dirs, 1);
}

/* Multi-column: cols[0] is the primary key. Vyto passes the two int[] buffers. */
void ct_sort_keys(void *h, const long long *cols, const long long *dirs, int nk) {
    CT *t = (CT *)h;
    if (!t || nk <= 0) return;
    int cbuf[64], dbuf[64];
    if (nk > 64) nk = 64;
    for (int i = 0; i < nk; i++) { cbuf[i] = (int)cols[i]; dbuf[i] = (int)dirs[i]; }
    ct_msort(t, cbuf, dbuf, nk);
}

/* ------------------------------------------------------------- filter */

void ct_reset_view(void *h) {
    CT *t = (CT *)h;
    if (!t) return;
    for (int64_t i = 0; i < t->nrow; i++) t->idx[i] = i;
    t->vlen = t->nrow;
}

/* ops: 0 eq, 1 ne, 2 lt, 3 le, 4 gt, 5 ge */
static int num_op(int op, double x, double y) {
    switch (op) {
        case 0: return x == y;
        case 1: return x != y;
        case 2: return x < y;
        case 3: return x <= y;
        case 4: return x > y;
        default: return x >= y;
    }
}

/* keep only visible rows where col <op> operand; compacts idx in place,
   preserving order (so filter composes with a prior sort). */
void ct_filter_f64(void *h, int col, int op, double operand) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    int64_t w = 0;
    for (int64_t k = 0; k < t->vlen; k++) {
        int64_t row = t->idx[k];
        if (num_op(op, cell_num(c, row), operand)) t->idx[w++] = row;
    }
    t->vlen = w;
}

void ct_filter_i64(void *h, int col, int op, long long operand) {
    ct_filter_f64(h, col, op, (double)operand);
}

/* case-sensitive substring (returns 1 if `needle` occurs in [p,p+n)). */
static int mem_contains(const char *p, int32_t n, const char *needle, int32_t nn) {
    if (nn == 0) return 1;
    if (nn > n) return 0;
    for (int32_t i = 0; i + nn <= n; i++) {
        if (p[i] == needle[0] && memcmp(p + i, needle, (size_t)nn) == 0) return 1;
    }
    return 0;
}

/* string filter. op 0 = equals, 6 = contains. */
void ct_filter_str(void *h, int col, int op, const char *needle) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol || !needle) return;
    Col *c = &t->cols[col];
    if (c->kind != CT_STR && c->kind != CT_CAT) { t->vlen = 0; return; }
    int32_t nn = (int32_t)strlen(needle);
    int64_t w = 0;
    for (int64_t k = 0; k < t->vlen; k++) {
        int64_t row = t->idx[k];
        const char *p;
        int32_t n;
        if (c->kind == CT_CAT) { p = cat_str(c, row, &n); }
        else { p = c->arena + c->soff[row]; n = c->slen[row]; }
        int hit = (op == 0) ? (n == nn && memcmp(p, needle, (size_t)nn) == 0)
                            : mem_contains(p, n, needle, nn);
        if (hit) t->idx[w++] = row;
    }
    t->vlen = w;
}

/* global text search: keep a row if `needle` occurs in ANY column's text
   (string arena, or the snprintf'd form of a scalar). Case-sensitive. */
void ct_search(void *h, const char *needle) {
    CT *t = (CT *)h;
    if (!t || !needle) return;
    int32_t nn = (int32_t)strlen(needle);
    if (nn == 0) return;
    char tmp[64];
    int64_t w = 0;
    for (int64_t k = 0; k < t->vlen; k++) {
        int64_t row = t->idx[k];
        int hit = 0;
        for (int col = 0; col < t->ncol && !hit; col++) {
            Col *c = &t->cols[col];
            if (c->kind == CT_STR) {
                hit = mem_contains(c->arena + c->soff[row], c->slen[row], needle, nn);
            } else if (c->kind == CT_CAT) {
                int32_t l; const char *p = cat_str(c, row, &l);
                hit = mem_contains(p, l, needle, nn);
            } else {
                int n;
                if (c->kind == CT_BOOL) n = snprintf(tmp, sizeof tmp, "%s", ((uint8_t *)c->data)[row] ? "true" : "false");
                else if (kind_is_int(c->kind)) n = snprintf(tmp, sizeof tmp, "%lld", (long long)cell_num(c, row));
                else n = snprintf(tmp, sizeof tmp, "%g", cell_num(c, row));
                hit = mem_contains(tmp, n, needle, nn);
            }
        }
        if (hit) t->idx[w++] = row;
    }
    t->vlen = w;
}

/* ------------------------------------------------------------- aggregate */

/* kind: 0 sum, 1 min, 2 max, 3 avg, 4 count. Over the visible rows. */
double ct_agg(void *h, int col, int kind) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return 0.0;
    if (kind == 4) return (double)t->vlen;
    Col *c = &t->cols[col];
    if (t->vlen == 0) return 0.0;
    double acc = cell_num(c, t->idx[0]);
    if (kind == 0 || kind == 3) {
        acc = 0.0;
        for (int64_t k = 0; k < t->vlen; k++) acc += cell_num(c, t->idx[k]);
        if (kind == 3) return acc / (double)t->vlen;
        return acc;
    }
    for (int64_t k = 1; k < t->vlen; k++) {
        double v = cell_num(c, t->idx[k]);
        if (kind == 1 && v < acc) acc = v;
        if (kind == 2 && v > acc) acc = v;
    }
    return acc;
}

/* Derive a new f64 column `name` = (col a) <op> (col b), computed over every
   row (not just the visible ones). op: 0 +, 1 -, 2 *, 3 /. Returns the new
   column index, or -1. */
int ct_derive(void *h, const char *name, int op, int a, int b) {
    CT *t = (CT *)h;
    if (!t || a < 0 || a >= t->ncol || b < 0 || b >= t->ncol) return -1;
    int nc = ct_add_col(h, name, CT_F64);
    if (nc < 0) return -1;
    Col *ca = &t->cols[a], *cb = &t->cols[b];
    double *out = (double *)t->cols[nc].data;
    for (int64_t r = 0; r < t->nrow; r++) {
        double x = cell_num(ca, r), y = cell_num(cb, r), v = 0.0;
        if (op == 0) v = x + y;
        else if (op == 1) v = x - y;
        else if (op == 2) v = x * y;
        else v = y != 0.0 ? x / y : 0.0;
        out[r] = v;
    }
    return nc;
}

/* ------------------------------------------------------------- free */

void ct_free(void *h) {
    CT *t = (CT *)h;
    if (!t) return;
    for (int c = 0; c < t->ncol; c++) {
        free(t->cols[c].name);
        free(t->cols[c].data);
        free(t->cols[c].soff);
        free(t->cols[c].slen);
        free(t->cols[c].arena);
        free(t->cols[c].doff);
        free(t->cols[c].dlen);
        free(t->cols[c].dhash);
    }
    free(t->cols);
    free(t->idx);
    free(t);
}
