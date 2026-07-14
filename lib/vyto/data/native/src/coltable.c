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

enum { CT_I64 = 0, CT_F64 = 1, CT_BOOL = 2, CT_STR = 3 };

/* one column: `data` is int64 / double / uint8 for the scalar kinds; the string
   kind uses soff/slen (per-row start offset + byte length) into a shared arena. */
typedef struct {
    int kind;
    char *name;
    void *data;        /* scalar kinds */
    int64_t *soff;     /* CT_STR: start offset of each row's bytes in arena */
    int32_t *slen;     /* CT_STR: byte length of each row's string */
    char *arena;       /* CT_STR: packed bytes for all rows */
    size_t alen, acap; /* CT_STR: arena used / capacity */
} Col;

typedef struct {
    Col *cols;
    int ncol, ccap;    /* column count / column-array capacity */
    int64_t nrow, cap; /* row count / row capacity of every column */
    int64_t *idx;      /* visible permutation, first `vlen` entries live */
    int64_t vlen;      /* number of visible rows (after filter) */
} CT;

/* ------------------------------------------------------------- allocation */

static size_t col_elem(int kind) {
    if (kind == CT_F64) return sizeof(double);
    if (kind == CT_BOOL) return 1;
    return sizeof(int64_t); /* CT_I64 */
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
    int64_t *ni = (int64_t *)realloc(t->idx, (size_t)nc * sizeof(int64_t));
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
    /* back-fill defaults for any rows that already exist */
    for (int64_t r = 0; r < t->nrow; r++) {
        if (kind == CT_STR) { col->soff[r] = (int64_t)col->alen; col->slen[r] = 0; }
        else if (kind == CT_F64) ((double *)col->data)[r] = 0.0;
        else if (kind == CT_BOOL) ((uint8_t *)col->data)[r] = 0;
        else ((int64_t *)col->data)[r] = 0;
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
        else if (col->kind == CT_F64) ((double *)col->data)[r] = 0.0;
        else if (col->kind == CT_BOOL) ((uint8_t *)col->data)[r] = 0;
        else ((int64_t *)col->data)[r] = 0;
    }
}

void ct_set_i64(void *h, int col, long long v) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->kind == CT_F64) ((double *)c->data)[t->nrow] = (double)v;
    else if (c->kind == CT_BOOL) ((uint8_t *)c->data)[t->nrow] = v ? 1 : 0;
    else ((int64_t *)c->data)[t->nrow] = (int64_t)v;
}

void ct_set_f64(void *h, int col, double v) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->kind == CT_I64) ((int64_t *)c->data)[t->nrow] = (int64_t)v;
    else if (c->kind == CT_BOOL) ((uint8_t *)c->data)[t->nrow] = v != 0.0 ? 1 : 0;
    else ((double *)c->data)[t->nrow] = v;
}

void ct_set_bool(void *h, int col, int v) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->kind == CT_BOOL) ((uint8_t *)c->data)[t->nrow] = v ? 1 : 0;
    else if (c->kind == CT_F64) ((double *)c->data)[t->nrow] = v ? 1.0 : 0.0;
    else ((int64_t *)c->data)[t->nrow] = v ? 1 : 0;
}

void ct_set_str(void *h, int col, const char *s, long n) {
    CT *t = (CT *)h;
    if (!t || col < 0 || col >= t->ncol) return;
    Col *c = &t->cols[col];
    if (c->kind != CT_STR || !s || n < 0) return;
    if (c->alen + (size_t)n > c->acap) {
        size_t na = c->acap ? c->acap : 256;
        while (na < c->alen + (size_t)n) na *= 2;
        char *nb = (char *)realloc(c->arena, na);
        if (!nb) return;
        c->arena = nb;
        c->acap = na;
    }
    c->soff[t->nrow] = (int64_t)c->alen;
    c->slen[t->nrow] = (int32_t)n;
    memcpy(c->arena + c->alen, s, (size_t)n);
    c->alen += (size_t)n;
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
    if (c->kind == CT_BOOL) return ((uint8_t *)c->data)[row] ? 1.0 : 0.0;
    if (c->kind == CT_I64) return (double)((int64_t *)c->data)[row];
    return 0.0;
}

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
    if (c->kind == CT_STR) {
        long n = c->slen[row];
        if (n > cap - 1) n = cap - 1;
        memcpy(buf, c->arena + c->soff[row], (size_t)n);
        buf[n] = 0;
        return n;
    }
    if (c->kind == CT_I64) return snprintf(buf, (size_t)cap, "%lld", (long long)((int64_t *)c->data)[row]);
    if (c->kind == CT_BOOL) { const char *s = ((uint8_t *)c->data)[row] ? "true" : "false"; long n = (long)strlen(s); memcpy(buf, s, (size_t)n + 1); return n; }
    return snprintf(buf, (size_t)cap, "%g", ((double *)c->data)[row]);
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
    if (c->kind == CT_STR) {
        int32_t la = c->slen[a], lb = c->slen[b];
        int32_t m = la < lb ? la : lb;
        int r = m > 0 ? memcmp(c->arena + c->soff[a], c->arena + c->soff[b], (size_t)m) : 0;
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
    int64_t *src = t->idx;
    int64_t *tmp = (int64_t *)malloc((size_t)n * sizeof(int64_t));
    if (!tmp) return;
    int64_t *buf = tmp, *cur = src;
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
        int64_t *sw = cur; cur = buf; buf = sw;
    }
    if (cur != src) { memcpy(src, cur, (size_t)n * sizeof(int64_t)); }
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
    if (c->kind != CT_STR) { t->vlen = 0; return; }
    int32_t nn = (int32_t)strlen(needle);
    int64_t w = 0;
    for (int64_t k = 0; k < t->vlen; k++) {
        int64_t row = t->idx[k];
        const char *p = c->arena + c->soff[row];
        int32_t n = c->slen[row];
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
            } else {
                int n;
                if (c->kind == CT_I64) n = snprintf(tmp, sizeof tmp, "%lld", (long long)((int64_t *)c->data)[row]);
                else if (c->kind == CT_BOOL) n = snprintf(tmp, sizeof tmp, "%s", ((uint8_t *)c->data)[row] ? "true" : "false");
                else n = snprintf(tmp, sizeof tmp, "%g", ((double *)c->data)[row]);
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
    }
    free(t->cols);
    free(t->idx);
    free(t);
}
