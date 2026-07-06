/* voltbind — generate Volt extern bindings from a C header.
 *
 *   voltbind <header.h> [--lib name]... [--filter prefix*]... [-I dir]... [--cc cc]
 *
 * Pipeline: preprocess the header with `cc -E -P` (and `-dM -E` for macros),
 * parse the *declarations only* with a best-effort C subset parser, map types
 * to Volt's FFI surface, and print a .vt module on stdout.
 *
 * Lossy by design: functions using varargs, function pointers, long double,
 * bitfields, or by-value structs with unmappable fields are skipped and
 * listed in a comment. Pointers to anything but char become rawptr.
 */
#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>

/* ---------------- C token stream ---------------- */

typedef enum { CT_EOF, CT_IDENT, CT_NUM, CT_STR, CT_CHR, CT_PUNCT } CTokKind;

typedef struct CTok {
    CTokKind kind;
    const char *s; /* ident text or punct text (interned) */
    int64_t ival;
    bool is_int;
} CTok;

static CTok *toks;
static int ntoks, captoks, pos;

static void tok_push(CTok t) {
    if (ntoks == captoks) {
        captoks = captoks ? captoks * 2 : 4096;
        CTok *nt = arena_alloc(&g_arena, sizeof(CTok) * (size_t)captoks);
        memcpy(nt, toks, sizeof(CTok) * (size_t)ntoks);
        toks = nt;
    }
    toks[ntoks++] = t;
}

static void tokenize(const char *src) {
    const char *p = src;
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (*p == '#') { while (*p && *p != '\n') p++; continue; } /* stray line markers */
        CTok t = {0};
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            t.kind = CT_IDENT;
            t.s = intern_n(s, (size_t)(p - s));
            tok_push(t);
            continue;
        }
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            const char *s = p;
            bool isint = true;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (isxdigit((unsigned char)*p)) p++;
            } else {
                while (isdigit((unsigned char)*p)) p++;
                if (*p == '.' || *p == 'e' || *p == 'E') {
                    isint = *p != '.' && *p != 'e' && *p != 'E';
                    while (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '+' || *p == '-')
                        p++;
                    isint = false;
                }
            }
            while (isalpha((unsigned char)*p)) p++; /* suffixes ULl f */
            t.kind = CT_NUM;
            t.is_int = isint;
            char buf[64];
            size_t n = (size_t)(p - s);
            if (n < sizeof buf) {
                memcpy(buf, s, n);
                buf[n] = 0;
                t.ival = strtoll(buf, NULL, 0);
            }
            t.s = intern_n(s, n);
            tok_push(t);
            continue;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p) p++;
            t.kind = CT_STR;
            tok_push(t);
            continue;
        }
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p) p++;
            t.kind = CT_CHR;
            tok_push(t);
            continue;
        }
        /* punctuation: only "..." matters as a multi-char unit */
        if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
            t.kind = CT_PUNCT;
            t.s = intern("...");
            p += 3;
            tok_push(t);
            continue;
        }
        t.kind = CT_PUNCT;
        t.s = intern_n(p, 1);
        p++;
        tok_push(t);
    }
    CTok eof = {0};
    eof.kind = CT_EOF;
    tok_push(eof);
}

static CTok *cur(void) { return &toks[pos]; }
static CTok *peek(int n) { return &toks[pos + n < ntoks ? pos + n : ntoks - 1]; }
static void adv(void) { if (pos < ntoks - 1) pos++; }
static bool is_p(const char *s) { return cur()->kind == CT_PUNCT && cur()->s == intern(s); }
static bool is_kw(const char *s) { return cur()->kind == CT_IDENT && cur()->s == intern(s); }
static bool eat_p(const char *s) { if (is_p(s)) { adv(); return true; } return false; }
static bool eat_kw(const char *s) { if (is_kw(s)) { adv(); return true; } return false; }

static void skip_balanced(const char *open, const char *close) {
    /* assumes cur() == open */
    int depth = 0;
    do {
        if (is_p(open)) depth++;
        else if (is_p(close)) depth--;
        adv();
    } while (depth > 0 && cur()->kind != CT_EOF);
}

static void skip_to_semi(void) {
    while (cur()->kind != CT_EOF) {
        if (is_p("{")) { skip_balanced("{", "}"); continue; }
        if (is_p(";")) { adv(); return; }
        adv();
    }
}

/* skip gcc noise words wherever they may appear */
static bool skip_noise(void) {
    bool any = false;
    for (;;) {
        if (eat_kw("__extension__") || eat_kw("__restrict") || eat_kw("__restrict__") ||
            eat_kw("restrict") || eat_kw("__inline") || eat_kw("__inline__") ||
            eat_kw("inline") || eat_kw("_Noreturn") || eat_kw("__volatile__") ||
            eat_kw("volatile") || eat_kw("register")) {
            any = true;
            continue;
        }
        if ((is_kw("__attribute__") || is_kw("__attribute") || is_kw("_Alignas") ||
             is_kw("__asm__") || is_kw("__asm") || is_kw("asm") || is_kw("__declspec")) ) {
            const char *name = cur()->s;
            adv();
            if (is_p("(")) skip_balanced("(", ")");
            /* an asm rename changes the link symbol — poison the declaration */
            if (name == intern("__asm__") || name == intern("__asm") || name == intern("asm"))
                return true; /* handled by caller via re-check; treat as noise */
            any = true;
            continue;
        }
        return any;
    }
}

/* ---------------- C types ---------------- */

typedef enum {
    B_UNKNOWN, B_VOID, B_BOOL, B_CHAR, B_SCHAR, B_UCHAR, B_SHORT, B_USHORT, B_INT, B_UINT,
    B_LONG, B_ULONG, B_LLONG, B_ULLONG, B_FLOAT, B_DOUBLE, B_LDOUBLE, B_STRUCT, B_UNION,
    B_ENUM, B_FNPTR,
} CBase;

typedef struct CType {
    CBase base;
    int ptr;                /* levels of indirection */
    const char *tag;        /* struct/union/enum tag or typedef-origin name */
} CType;

typedef struct CField {
    const char *name;
    CType type;
} CField;

typedef struct CStruct {
    const char *name;       /* tag or typedef name */
    CField fields[64];
    int nfields;
    bool mappable;
    bool has_body;
    bool emitted;
    bool value_used;
    struct CStruct *next;
} CStruct;

typedef struct CParam {
    const char *name;
    CType type;
} CParam;

typedef struct CFn {
    const char *name;
    CType ret;
    CParam params[32];
    int nparams;
    struct CFn *next;
} CFn;

typedef struct CTypedef {
    const char *name;
    CType type;
    struct CTypedef *next;
} CTypedef;

typedef struct CConst {
    const char *name;
    int64_t val;
    struct CConst *next;
} CConst;

typedef struct Skip {
    const char *name;
    const char *why;
    struct Skip *next;
} Skip;

static CStruct *structs, **structs_tail = &structs;
static CFn *fns, **fns_tail = &fns;
static CTypedef *typedefs;
static CConst *consts, **consts_tail = &consts;
static Skip *skips, **skips_tail = &skips;

static void add_skip(const char *name, const char *why) {
    for (Skip *s = skips; s; s = s->next)
        if (s->name == name) return;
    Skip *s = NEW(Skip);
    s->name = name;
    s->why = why;
    *skips_tail = s;
    skips_tail = &s->next;
}

static CStruct *find_struct(const char *name) {
    for (CStruct *s = structs; s; s = s->next)
        if (s->name == name) return s;
    return NULL;
}

static CStruct *get_struct(const char *name) {
    CStruct *s = find_struct(name);
    if (s) return s;
    s = NEW(CStruct);
    s->name = name;
    s->mappable = true;
    *structs_tail = s;
    structs_tail = &s->next;
    return s;
}

static CTypedef *find_typedef(const char *name) {
    for (CTypedef *t = typedefs; t; t = t->next)
        if (t->name == name) return t;
    return NULL;
}

/* ---------------- type parsing ---------------- */

static bool parse_struct_body(CStruct *sc);

/* parse base type specifiers; returns false if nothing type-like here */
static bool parse_base(CType *out) {
    int n_long = 0, n_short = 0, n_signed = 0, n_unsigned = 0;
    bool got_int = false, got_char = false, got_float = false, got_double = false;
    bool got_void = false, got_bool = false, any = false;
    out->base = B_UNKNOWN;
    out->ptr = 0;
    out->tag = NULL;
    for (;;) {
        skip_noise();
        /* qualifiers alone don't make a type: "const Foo *x" must still
           resolve Foo through the typedef table below */
        if (eat_kw("const") || eat_kw("__const")) continue;
        if (is_kw("struct") || is_kw("union")) {
            bool is_union = is_kw("union");
            adv();
            skip_noise();
            const char *tag = NULL;
            if (cur()->kind == CT_IDENT) { tag = cur()->s; adv(); }
            CStruct *sc = NULL;
            if (tag) sc = get_struct(tag);
            if (is_p("{")) {
                if (!sc) {
                    sc = get_struct(intern(arena_printf(&g_arena, "_anon%d", pos)));
                }
                if (is_union) { sc->mappable = false; skip_balanced("{", "}"); sc->has_body = true; }
                else if (!parse_struct_body(sc)) sc->mappable = false;
            }
            out->base = is_union ? B_UNION : B_STRUCT;
            out->tag = sc ? sc->name : tag;
            return true;
        }
        if (is_kw("enum")) {
            adv();
            skip_noise();
            if (cur()->kind == CT_IDENT) { out->tag = cur()->s; adv(); }
            if (is_p("{")) {
                adv();
                int64_t next = 0;
                bool valid = true;
                while (!is_p("}") && cur()->kind != CT_EOF) {
                    if (cur()->kind != CT_IDENT) { adv(); continue; }
                    const char *en = cur()->s;
                    adv();
                    if (eat_p("=")) {
                        bool neg = eat_p("-");
                        if (cur()->kind == CT_NUM && cur()->is_int) {
                            next = neg ? -cur()->ival : cur()->ival;
                            adv();
                            valid = true;
                            /* tolerate simple "A = B | C" etc: bail on extra tokens */
                            if (!is_p(",") && !is_p("}")) {
                                valid = false;
                                while (!is_p(",") && !is_p("}") && cur()->kind != CT_EOF) adv();
                            }
                        } else {
                            valid = false;
                            while (!is_p(",") && !is_p("}") && cur()->kind != CT_EOF) adv();
                        }
                    }
                    if (valid && en[0] != '_') {
                        CConst *cc = NEW(CConst);
                        cc->name = en;
                        cc->val = next;
                        *consts_tail = cc;
                        consts_tail = &cc->next;
                    }
                    next++;
                    eat_p(",");
                }
                eat_p("}");
            }
            out->base = B_ENUM;
            return true;
        }
        if (eat_kw("void")) { got_void = true; any = true; continue; }
        if (eat_kw("_Bool")) { got_bool = true; any = true; continue; }
        if (eat_kw("char")) { got_char = true; any = true; continue; }
        if (eat_kw("int")) { got_int = true; any = true; continue; }
        if (eat_kw("float")) { got_float = true; any = true; continue; }
        if (eat_kw("double")) { got_double = true; any = true; continue; }
        if (eat_kw("long")) { n_long++; any = true; continue; }
        if (eat_kw("short")) { n_short++; any = true; continue; }
        if (eat_kw("signed") || eat_kw("__signed__")) { n_signed++; any = true; continue; }
        if (eat_kw("unsigned")) { n_unsigned++; any = true; continue; }
        break;
    }
    if (!any) {
        /* typedef name? */
        if (cur()->kind == CT_IDENT) {
            CTypedef *td = find_typedef(cur()->s);
            if (td) {
                adv();
                *out = td->type;
                return true;
            }
        }
        return false;
    }
    if (got_void) out->base = B_VOID;
    else if (got_bool) out->base = B_BOOL;
    else if (got_char) out->base = n_unsigned ? B_UCHAR : (n_signed ? B_SCHAR : B_CHAR);
    else if (got_float) out->base = B_FLOAT;
    else if (got_double) out->base = n_long ? B_LDOUBLE : B_DOUBLE;
    else if (n_short) out->base = n_unsigned ? B_USHORT : B_SHORT;
    else if (n_long >= 2) out->base = n_unsigned ? B_ULLONG : B_LLONG;
    else if (n_long == 1) out->base = n_unsigned ? B_ULONG : B_LONG;
    else out->base = n_unsigned ? B_UINT : B_INT;
    (void)got_int;
    return true;
}

/* parse pointer stars + optional name; arrays decay to pointers.
   Function-pointer declarators become B_FNPTR. Returns declared name or NULL. */
static const char *parse_declarator(CType *t) {
    for (;;) {
        skip_noise();
        if (eat_p("*")) { t->ptr++; eat_kw("const"); continue; }
        break;
    }
    skip_noise();
    if (is_p("(")) { /* function pointer: RET (*name)(...) */
        skip_balanced("(", ")");
        if (is_p("(")) skip_balanced("(", ")");
        t->base = B_FNPTR;
        t->ptr = 0;
        return NULL;
    }
    const char *name = NULL;
    if (cur()->kind == CT_IDENT) { name = cur()->s; adv(); }
    skip_noise();
    while (is_p("[")) { skip_balanced("[", "]"); t->ptr++; }
    return name;
}

static bool field_mappable(const CType *t);

static bool parse_struct_body(CStruct *sc) {
    /* cur() == '{' */
    adv();
    sc->has_body = true;
    bool ok = true;
    while (!is_p("}") && cur()->kind != CT_EOF) {
        CType base;
        if (!parse_base(&base)) { ok = false; skip_to_semi(); continue; }
        /* one or more declarators */
        do {
            CType ft = base;
            const char *fname = parse_declarator(&ft);
            skip_noise();
            if (is_p(":")) { /* bitfield */
                ok = false;
                skip_to_semi();
                goto next_field_line;
            }
            if (!fname || !field_mappable(&ft) || sc->nfields >= 64) ok = false;
            else {
                sc->fields[sc->nfields].name = fname;
                sc->fields[sc->nfields].type = ft;
                sc->nfields++;
            }
        } while (eat_p(","));
        if (!eat_p(";")) { ok = false; skip_to_semi(); }
    next_field_line:;
    }
    eat_p("}");
    if (!ok) sc->mappable = false;
    return ok;
}

/* ---------------- type mapping ---------------- */

static const char *volt_keywords[] = {
    "fn", "let", "const", "struct", "class", "extends", "virtual", "override", "init",
    "deinit", "new", "weak", "import", "export", "from", "extern", "if", "else", "while",
    "for", "in", "return", "break", "continue", "true", "false", "null", "this", "as",
    "super", "Map", "int", "float", "bool", "byte", "string", "cstring", "rawptr", "void",
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "print", "panic",
    "str", "len", "push", "pop", "set", "get", "has", "remove", "cstr", NULL,
};

static bool is_volt_keyword(const char *s) {
    for (int i = 0; volt_keywords[i]; i++)
        if (s == intern(volt_keywords[i])) return true;
    return false;
}

/* map a C type to a Volt type name; NULL = unmappable */
static const char *volt_type(const CType *t, bool is_ret) {
    if (t->base == B_FNPTR) return "rawptr"; /* pass thunks from cthunk() */
    if (t->ptr > 0) {
        if ((t->base == B_CHAR || t->base == B_SCHAR) && t->ptr == 1) return "cstring";
        return "rawptr";
    }
    switch (t->base) {
    case B_VOID: return is_ret ? "void" : NULL;
    case B_BOOL: return "bool";
    case B_CHAR: case B_SCHAR: return "i8";
    case B_UCHAR: return "u8";
    case B_SHORT: return "i16";
    case B_USHORT: return "u16";
    case B_INT: return "i32";
    case B_UINT: return "u32";
    case B_LONG: return "i64";  /* LP64; Windows would need i32 — good enough for v0.1 */
    case B_ULONG: return "u64";
    case B_LLONG: return "i64";
    case B_ULLONG: return "u64";
    case B_FLOAT: return "f32";
    case B_DOUBLE: return "f64";
    case B_ENUM: return "i32";
    case B_STRUCT: {
        CStruct *sc = t->tag ? find_struct(t->tag) : NULL;
        if (sc && sc->has_body && sc->mappable) {
            sc->value_used = true;
            return sc->name;
        }
        return NULL;
    }
    default: return NULL;
    }
}

static bool field_mappable(const CType *t) {
    if (t->base == B_FNPTR) return true; /* maps to rawptr */
    if (t->base == B_LDOUBLE || t->base == B_UNION || t->base == B_UNKNOWN) return false;
    if (t->ptr == 0 && t->base == B_VOID) return false;
    return true;
}

/* ---------------- top-level parse ---------------- */

static void parse_typedef(void) {
    adv(); /* typedef */
    CType base;
    if (!parse_base(&base)) { skip_to_semi(); return; }
    do {
        CType t = base;
        const char *name = parse_declarator(&t);
        if (name) {
            CTypedef *td = NEW(CTypedef);
            td->name = name;
            td->type = t;
            td->next = typedefs;
            typedefs = td;
            /* "typedef struct {...} Foo" names an anonymous struct */
            if (t.base == B_STRUCT && t.ptr == 0 && t.tag) {
                CStruct *sc = find_struct(t.tag);
                if (sc && sc->name != name && strncmp(sc->name, "_anon", 5) == 0)
                    sc->name = name;
            }
        }
    } while (eat_p(","));
    skip_to_semi();
}

static void parse_top(void) {
    while (cur()->kind != CT_EOF) {
        skip_noise();
        if (eat_p(";")) continue;
        if (is_kw("typedef")) { parse_typedef(); continue; }
        eat_kw("extern");
        eat_kw("static");
        skip_noise();
        CType base;
        int save = pos;
        if (!parse_base(&base)) { skip_to_semi(); continue; }
        (void)save;
        /* struct/union/enum definition with no declarator */
        skip_noise();
        if (is_p(";")) { adv(); continue; }
        CType t = base;
        const char *name = parse_declarator(&t);
        if (name && is_p("(")) {
            /* function declaration */
            CFn fn = {0};
            fn.name = name;
            fn.ret = t;
            adv();
            bool bad = false;
            if (is_kw("void") && peek(1)->kind == CT_PUNCT && peek(1)->s == intern(")")) {
                adv();
            }
            while (!is_p(")") && cur()->kind != CT_EOF) {
                if (is_p("...")) { bad = true; add_skip(name, "varargs"); adv(); continue; }
                CType pbase;
                if (!parse_base(&pbase)) { bad = true; add_skip(name, "unparsable parameter"); skip_to_semi(); goto done_decl; }
                CType pt = pbase;
                const char *pname = parse_declarator(&pt);
                if (fn.nparams < 32) {
                    fn.params[fn.nparams].name = pname;
                    fn.params[fn.nparams].type = pt;
                    fn.nparams++;
                } else bad = true;
                if (!eat_p(",")) break;
            }
            eat_p(")");
            skip_noise();
            if (is_p("{")) { skip_balanced("{", "}"); continue; } /* inline definition: no symbol */
            skip_to_semi();
            if (!bad) {
                /* dedupe */
                bool dup = false;
                for (CFn *f = fns; f; f = f->next)
                    if (f->name == fn.name) dup = true;
                if (!dup) {
                    CFn *nf = NEW(CFn);
                    *nf = fn;
                    *fns_tail = nf;
                    fns_tail = &nf->next;
                }
            }
        } else {
            /* variable or something else — skip */
            skip_to_semi();
        }
    done_decl:;
    }
}

/* ---------------- macros (#define ints) ---------------- */

static void parse_defines(const char *src) {
    const char *p = src;
    while (*p) {
        if (strncmp(p, "#define ", 8) == 0) {
            p += 8;
            const char *ns = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            size_t nlen = (size_t)(p - ns);
            if (nlen && *p != '(' && ns[0] != '_') { /* skip fn-like and reserved */
                while (*p == ' ' || *p == '\t') p++;
                const char *vs = p;
                /* allow one layer of parens */
                bool paren = *p == '(';
                if (paren) { p++; vs = p; }
                bool neg = *p == '-';
                if (neg) p++;
                const char *ds = p;
                if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                    p += 2;
                    while (isxdigit((unsigned char)*p)) p++;
                } else
                    while (isdigit((unsigned char)*p)) p++;
                bool got = p > ds;
                while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') p++;
                if (paren && *p == ')') p++;
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (got && *p == '\n') {
                    char buf[64];
                    size_t dlen = (size_t)(p - vs);
                    /* strip suffixes from the copied text */
                    size_t k = 0;
                    for (size_t i = 0; i < dlen && k < sizeof buf - 1; i++)
                        if (vs[i] != 'u' && vs[i] != 'U' && vs[i] != 'l' && vs[i] != 'L' &&
                            vs[i] != ')')
                            buf[k++] = vs[i];
                    buf[k] = 0;
                    CConst *cc = NEW(CConst);
                    cc->name = intern_n(ns, nlen);
                    cc->val = strtoll(buf, NULL, 0);
                    *consts_tail = cc;
                    consts_tail = &cc->next;
                }
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

/* ---------------- filters & output ---------------- */

static const char *filters[64];
static int nfilters;

static bool pass_filter(const char *name) {
    if (name[0] == '_') return false;
    if (nfilters == 0) return true;
    for (int i = 0; i < nfilters; i++) {
        const char *f = filters[i];
        size_t fl = strlen(f);
        if (fl && f[fl - 1] == '*') {
            if (strncmp(name, f, fl - 1) == 0) return true;
        } else if (strcmp(name, f) == 0) return true;
    }
    return false;
}

static char *run_capture(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) fatal("cannot run: %s", cmd);
    SBuf sb;
    sb_init(&sb);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) sb_putc(&sb, buf[i]);
    int rc = pclose(f);
    if (rc != 0) fatal("preprocessor failed (%s)", cmd);
    char *out = arena_strdup(&g_arena, sb.data);
    sb_free(&sb);
    return out;
}

int main(int argc, char **argv) {
    const char *header = NULL, *cc = "cc";
    const char *libs[16];
    int nlibs = 0;
    SBuf incs;
    sb_init(&incs);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) libs[nlibs++] = argv[++i];
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filters[nfilters++] = argv[++i];
        else if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc) cc = argv[++i];
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) sb_printf(&incs, " -I'%s'", argv[++i]);
        else if (!header) header = argv[i];
        else fatal("unexpected argument '%s'", argv[i]);
    }
    if (!header)
        fatal("usage: voltbind <header.h> [--lib name] [--filter prefix*] [-I dir] [--cc cc]");

    char *pp = run_capture(arena_printf(&g_arena, "%s -E -P%s '%s' 2>/dev/null", cc, incs.data, header));
    char *dm = run_capture(arena_printf(&g_arena, "%s -dM -E%s '%s' 2>/dev/null", cc, incs.data, header));

    tokenize(pp);
    parse_top();
    parse_defines(dm);

    /* select functions */
    printf("// Generated by voltbind from %s — do not edit.\n", header);
    printf("// voltbind");
    for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n\n");
    for (int i = 0; i < nlibs; i++) printf("#link \"%s\"\n", libs[i]);
    if (nlibs) printf("\n");

    /* pass 1: decide which functions are emitted; marks value-used structs */
    typedef struct { CFn *fn; } Sel;
    Sel sels[4096];
    int nsel = 0;
    for (CFn *f = fns; f; f = f->next) {
        if (!pass_filter(f->name)) continue;
        if (is_volt_keyword(f->name)) { add_skip(f->name, "name is a Volt keyword"); continue; }
        bool ok = volt_type(&f->ret, true) != NULL;
        if (!ok) add_skip(f->name, "unmappable return type");
        for (int i = 0; ok && i < f->nparams; i++) {
            if (f->params[i].type.base == B_VOID && f->params[i].type.ptr == 0 && f->nparams == 1) {
                f->nparams = 0;
                break;
            }
            if (!volt_type(&f->params[i].type, false)) {
                ok = false;
                add_skip(f->name, "unmappable parameter type");
            }
        }
        if (ok && nsel < 4096) sels[nsel++].fn = f;
    }

    printf("extern \"C\" {\n");
    /* structs used by value */
    for (CStruct *s = structs; s; s = s->next) {
        if (!s->value_used || !s->mappable || !s->has_body || s->emitted) continue;
        if (is_volt_keyword(s->name) || s->name[0] == '_') continue;
        s->emitted = true;
        printf("    struct %s {", s->name);
        for (int i = 0; i < s->nfields; i++) {
            const char *vt = volt_type(&s->fields[i].type, false);
            const char *fn2 = s->fields[i].name;
            printf(" %s: %s;", is_volt_keyword(fn2) ? arena_printf(&g_arena, "%s_", fn2) : fn2,
                   vt ? vt : "rawptr");
        }
        printf(" }\n");
    }
    for (int i = 0; i < nsel; i++) {
        CFn *f = sels[i].fn;
        printf("    fn %s(", f->name);
        for (int p = 0; p < f->nparams; p++) {
            const char *pn = f->params[p].name;
            if (!pn || is_volt_keyword(pn)) pn = arena_printf(&g_arena, "a%d", p);
            printf("%s%s: %s", p ? ", " : "", pn, volt_type(&f->params[p].type, false));
        }
        const char *rt = volt_type(&f->ret, true);
        if (strcmp(rt, "void") == 0) printf(");\n");
        else printf("): %s;\n", rt);
    }
    printf("}\n");

    /* consts from enums + #defines */
    bool any_const = false;
    for (CConst *cc2 = consts; cc2; cc2 = cc2->next) {
        if (!pass_filter(cc2->name) || is_volt_keyword(cc2->name)) continue;
        /* dedupe: first wins */
        bool dup = false;
        for (CConst *prev = consts; prev != cc2; prev = prev->next)
            if (prev->name == cc2->name) dup = true;
        if (dup) continue;
        if (!any_const) { printf("\n"); any_const = true; }
        printf("const %s: i64 = %lld;\n", cc2->name, (long long)cc2->val);
    }

    if (skips) {
        printf("\n// Skipped (not representable in Volt v0.1):\n");
        for (Skip *s = skips; s; s = s->next)
            if (pass_filter(s->name)) printf("//   %s — %s\n", s->name, s->why);
    }
    return 0;
}
