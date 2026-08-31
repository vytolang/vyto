#ifndef VYTO_AST_H
#define VYTO_AST_H

#include "util.h"

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct Module Module;
typedef struct FnDecl FnDecl;
typedef struct ClassDecl ClassDecl;
typedef struct StructDecl StructDecl;
typedef struct EnumDecl EnumDecl;

/* ---------------- types ---------------- */

typedef enum TypeKind {
    TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_BYTE,
    TY_I8, TY_I16, TY_I32, TY_I64, TY_U8, TY_U16, TY_U32, TY_U64, TY_F32, TY_F64,
    TY_CLONG, TY_CULONG,  /* C long / unsigned long: target-width, emit as raw C `long` */
    TY_STRING, TY_CSTRING, TY_RAWPTR, TY_NULL,
    TY_TYPARAM, /* reference to an in-scope generic type parameter; ->name */
    TY_NAMED,   /* unresolved identifier, resolved by checker */
    TY_ENUM,    /* ->edecl; an int at runtime, distinct to the checker */
    TY_STRUCT,  /* ->sdecl */
    TY_CLASS,   /* ->cdecl; ->weak flag on reference site */
    TY_ARRAY,   /* ->elem */
    TY_MAP,     /* ->elem = value type (keys always string) */
    TY_FN,      /* ->params/->nparams, ->ret */
} TypeKind;

struct Type {
    TypeKind kind;
    bool weak;
    /* This VALUE may be null and must be checked before it is dereferenced.
       Two sources, and the dereference rule treats them identically:
         - the parser, from a declared `T?`;
         - the checker, when a weak field is loaded — a weak slot reads as null
           once its target is gone, so every load of one is an optional.
       For a weak load it is a property of the loaded value rather than of the
       declaration: `weak` says how the slot is stored, `nullable` says what the
       reader has to do about it.
       Ignored by type_identical: a nullable T is still a T for assignment. */
    bool nullable;
    /* Did `nullable` come from a weak load rather than a declared `T?`? Only
       the diagnostic differs — the weak load clears `weak` above, so without
       this the message cannot tell the two apart. */
    bool from_weak;
    Type *elem;         /* array elem / map value */
    Type *ret;          /* fn return */
    Type **params;      /* fn params */
    int nparams;
    const char *name;   /* TY_NAMED */
    Loc loc;
    StructDecl *sdecl;
    ClassDecl *cdecl;
    EnumDecl *edecl;
};

static inline bool type_is_ref(const Type *t) {
    switch (t->kind) {
    case TY_STRING: case TY_CLASS: case TY_ARRAY: case TY_MAP: case TY_FN: case TY_NULL:
        return true;
    default:
        return false;
    }
}
static inline bool type_is_int(const Type *t) {
    switch (t->kind) {
    case TY_INT: case TY_BYTE:
    case TY_I8: case TY_I16: case TY_I32: case TY_I64:
    case TY_U8: case TY_U16: case TY_U32: case TY_U64:
    case TY_CLONG: case TY_CULONG:
        return true;
    default: return false;
    }
}
static inline bool type_is_float(const Type *t) {
    return t->kind == TY_FLOAT || t->kind == TY_F32 || t->kind == TY_F64;
}
static inline bool type_is_num(const Type *t) { return type_is_int(t) || type_is_float(t); }

/* ---------------- expressions ---------------- */

typedef enum ExprKind {
    EX_INT, EX_FLOAT, EX_STR, EX_BOOL, EX_NULL,
    EX_IDENT, EX_THIS,
    EX_BIN,      /* op, lhs, rhs */
    EX_UN,       /* op, lhs */
    EX_ASSIGN,   /* op (T_ASSIGN..T_PERCENTEQ), lhs, rhs */
    EX_CALL,     /* lhs = callee, args */
    EX_INDEX,    /* lhs[rhs] */
    EX_MEMBER,   /* lhs.name */
    EX_NEW,      /* name/cdecl + args, or map_new */
    EX_ARROW,    /* closure literal: fn_lit */
    EX_ARRAYLIT, /* args = elements */
    EX_TSTR,     /* template literal: args alternate chunk/hole, always odd,
                    args[even] are EX_STR chunks, args[odd] are the holes */
    EX_AS,       /* lhs as cast_type */
    EX_STRCONV,  /* checker-inserted: to-string of lhs */
} ExprKind;

typedef enum {
    /* resolved meaning of an EX_IDENT / EX_MEMBER / EX_CALL, set by checker */
    REF_NONE, REF_LOCAL, REF_PARAM, REF_GLOBAL_FN, REF_CONST, REF_FIELD,
    REF_METHOD, REF_BUILTIN, REF_CAPTURE, REF_EXTERN_FN,
    REF_STATIC_FN,          /* class static: ->method, direct call, no receiver */
    REF_METHOD_VAL, /* obj.method as a value: closure bound to obj */
} RefKind;

typedef struct Local Local; /* defined in check.h */

struct Expr {
    ExprKind kind;
    Loc loc;
    Type *type;         /* set by checker */
    int op;             /* TokKind for BIN/UN/ASSIGN */
    Expr *lhs, *rhs;
    Expr **args;
    int nargs;
    const char **arg_names; /* per-arg label for named arguments; NULL if none used */
    int64_t ival;
    double fval;
    const char *sval;   /* string literal bytes */
    size_t slen;
    const char *name;   /* ident / member name (interned) */
    Type *cast_type;    /* EX_AS, EX_NEW(map) */

    /* checker annotations */
    RefKind ref;
    Local *local;           /* REF_LOCAL/REF_PARAM/REF_CAPTURE */
    Decl *decl;             /* REF_GLOBAL_FN/REF_CONST/REF_EXTERN_FN target */
    ClassDecl *cls;         /* EX_NEW class; REF_FIELD/REF_METHOD owner */
    StructDecl *sd;         /* REF_FIELD on struct */
    FnDecl *method;         /* REF_METHOD/REF_METHOD_VAL target */
    EnumDecl *edecl;        /* B_ENUM_PARSE/B_ENUM_HAS: the enum asked, which is
                               the *type* and so is not on any operand */
    int builtin;            /* BuiltinKind for REF_BUILTIN */
    bool is_super_call;     /* super.init(...) */
    bool region_local;      /* EX_NEW resolved to arena allocation (region > 0) */
    const char *region_name; /* EX_NEW `new@name`: target arena name (interned); NULL = innermost */
    int region;             /* region id an expression's value lives in: 0 = heap, else arena id */
    FnDecl *fn_lit;         /* EX_ARROW body as a synthetic FnDecl */
    Type **type_args;       /* explicit f<T,...>(...) / New<T>(...) type args */
    int ntype_args;
};

typedef enum BuiltinKind {
    B_NONE, B_PRINT, B_PANIC, B_STR,
    B_LEN,          /* .len on string/array/map */
    B_ENUM_NAME,    /* .name() on an enum value */
    B_ENUM_PARSE,   /* Enum.parse(s) — the variant, panics if the name is none */
    B_ENUM_HAS,     /* Enum.has(s) — does that name a variant */
    B_PUSH, B_POP,  /* array */
    B_MAP_SET, B_MAP_GET, B_MAP_HAS, B_MAP_REMOVE,
    B_CSTR,         /* string.cstr() */
    B_SLICE,        /* string.slice(lo, hi) */
    B_CTHUNK,       /* cthunk(closure): C callback ptr, userdata first */
    B_CTHUNK_LAST,  /* cthunk_last(closure): userdata last */
    B_READFILE,     /* readfile(path): string */
    B_READLINES,    /* readlines(path): string[] */
    B_LISTDIR,      /* listdir(path): string[] */
    B_ISDIR,        /* isdir(path): bool */
    B_WRITEFILE,    /* writefile(path, data): bool */
    B_APPENDFILE,   /* appendfile(path, data): bool */
    B_BYTES,        /* bytes(n): byte[] — zeroed, sized buffer for FFI */
    B_BYTE_COMP,    /* byte_comp(a, alo, ahi, b, blo, bhi): int — lexicographic, no alloc */
    B_STRBYTES,     /* strbytes(b, n): string — first n bytes of a byte[] (no strlen) */
    B_ARGS,         /* args(): string[] — command-line arguments (excl. program name) */
    /* int methods */
    B_INT_ABS, B_INT_MIN, B_INT_MAX, B_INT_CLAMP, B_INT_SIGN, B_INT_POW, B_INT_GCD,
    B_INT_TO_FLOAT, B_INT_TO_STRING,
    /* float methods */
    B_FLT_ABS, B_FLT_MIN, B_FLT_MAX, B_FLT_CLAMP, B_FLT_FLOOR, B_FLT_CEIL, B_FLT_ROUND,
    B_FLT_TRUNC, B_FLT_SQRT, B_FLT_POW, B_FLT_TO_INT, B_FLT_IS_NAN,
    /* string methods */
    B_STR_IS_EMPTY, B_STR_CONTAINS, B_STR_STARTS_WITH, B_STR_ENDS_WITH,
    B_STR_INDEX_OF, B_STR_LAST_INDEX_OF, B_STR_COUNT, B_STR_CHAR_AT,
    B_STR_TO_UPPER, B_STR_TO_LOWER, B_STR_TRIM, B_STR_TRIM_START, B_STR_TRIM_END,
    B_STR_REPEAT, B_STR_PAD_START, B_STR_PAD_END, B_STR_REPLACE, B_STR_REVERSE,
    B_STR_SPLIT, B_STR_LINES, B_STR_TO_INT, B_STR_TO_FLOAT, B_STR_TO_FLOAT_AT,
    /* array methods */
    B_ARR_FIRST, B_ARR_LAST, B_ARR_NTH, B_ARR_IS_EMPTY, B_ARR_CONTAINS, B_ARR_INDEX_OF,
    B_ARR_REVERSE, B_ARR_CLEAR, B_ARR_INSERT, B_ARR_REMOVE_AT, B_ARR_EXTEND,
    B_ARR_CONCAT, B_ARR_SLICE, B_ARR_FILL, B_ARR_JOIN, B_ARR_RESERVE,
    /* array higher-order methods (closure args) */
    B_ARR_MAP, B_ARR_FILTER, B_ARR_REDUCE, B_ARR_EACH, B_ARR_FIND_INDEX,
    B_ARR_ANY, B_ARR_ALL, B_ARR_SORT,
    /* map methods */
    B_MAP_KEYS, B_MAP_VALUES, B_MAP_GET_OR, B_MAP_IS_EMPTY, B_MAP_CLEAR, B_MAP_MERGE,
} BuiltinKind;

/* ---------------- statements ---------------- */

typedef enum StmtKind {
    ST_LET, ST_EXPR, ST_IF, ST_WHILE, ST_FOR_RANGE, ST_FOR_EACH, ST_FOR_ITER,
    ST_RETURN, ST_BREAK, ST_CONTINUE, ST_BLOCK, ST_SWITCH,
    ST_ARENA,   /* arena [name] { body } — lexical region (uses Stmt.name/body) */
} StmtKind;

/* One arm of a switch. `nvalues == 0 && is_default` is the default arm; the
   values of a multi-value arm (`case 2, 3:`) all run the same body. */
/* Set by the checker when a switch over an enum covers every variant: such a
   switch is total, so it counts as returning on every path even with no
   default arm. */
typedef struct SwitchArm {
    Loc loc;
    Expr **values;
    int nvalues;
    bool is_default;
    Stmt **body;
    int nbody;
} SwitchArm;

struct Stmt {
    StmtKind kind;
    Loc loc;
    /* let */
    const char *name;
    Type *decl_type;    /* NULL = infer */
    Expr *init;
    Local *local;       /* set by checker */
    /* expr/if/while/return */
    Expr *expr;         /* condition or expression or return value */
    Stmt **body;        /* block statements / then-branch */
    int nbody;
    Stmt **els;         /* else-branch */
    int nels;
    /* for */
    Expr *range_lo, *range_hi; /* ST_FOR_RANGE */
    Expr *iter;                /* ST_FOR_EACH: array expr. ST_FOR_ITER: container expr */
    /* ST_FOR_ITER: the desugared index loop, built and checked by the checker.
       `for (let x in c)` becomes `c.len()` / `c.at(i)` over hidden locals. */
    Local *iter_local;         /* holds the container for the whole loop */
    Local *index_local;        /* the loop counter */
    Expr *len_call;            /* $c.len() */
    Expr *at_call;             /* $c.at($i) */
    /* switch: subject is `expr` */
    SwitchArm *arms;
    int narms;
    bool switch_total;         /* checker: enum switch covering every variant */
    int region;                /* ST_ARENA: region id assigned by checker (names _rgn<id>) */
};

/* ---------------- declarations ---------------- */

typedef struct Param {
    const char *name;
    Type *type;
    Loc loc;
    Expr *def;    /* default argument: literal-only, NULL if none */
    Local *local; /* set by checker */
} Param;

typedef struct Capture {
    Local *src;       /* local/param in enclosing fn */
    const char *name;
    Type *type;
} Capture;

struct FnDecl {
    const char *name;       /* NULL for arrows / deinit */
    Loc loc;
    Param *params;
    int nparams;
    Type *ret;              /* TY_VOID default */
    Stmt **body;
    int nbody;
    bool is_virtual, is_override, is_extern, is_builder;
    bool is_static;         /* class static: no receiver, direct call */
    ClassDecl *owner;       /* method owner, NULL for free fn */
    StructDecl *sowner;     /* struct method owner, NULL otherwise */
    Module *module;
    int vslot;              /* vtable slot if virtual, else -1 */
    /* checker outputs */
    Local *locals;          /* linked list of all locals incl. params */
    int ntemps;
    /* escape summary (--regions): esc_param[i] true if param i may escape this
       fn (returned / stored into a heap object, array, map or global / captured
       / passed to an escaping position). esc_this likewise for a method's `this`.
       Conservative: unknown/indirect/virtual callees escape their args. */
    bool *esc_param;        /* length nparams; NULL until computed */
    bool esc_this;
    bool esc_computed;      /* summary fixpoint has run */
    bool has_region;        /* body contains >=1 region_local allocation */
    /* closures */
    Capture *captures;
    int ncaptures;
    int arrow_id;           /* unique id for arrows within module */
    struct FnDecl *parent_fn; /* lexically enclosing fn, for arrows */
    /* generics: template if ntyparams > 0; instance if generic_origin != NULL */
    const char **typarams;
    int ntyparams;
    struct FnDecl *generic_origin;  /* template this instance was cloned from */
    Type **type_args;               /* instance's concrete type arguments */
    struct FnDecl *next_inst;       /* instantiation-cache list, hung off the origin */
    Decl *inst_decl;                /* synthetic Decl wrapping a fn instance */
    Loc first_use;                  /* first instantiating call site (diagnostics) */
    bool sig_resolved;              /* template sig resolved on demand (idempotent) */
    const char *cname;              /* memoized mangled name (instances) */
    /* emit: memoized fn_is_inlinable() — 0 unasked, 1 yes, -1 no. Small pure
       value-struct methods go in the module header as `static inline` so other
       modules can inline them (src/emit.c). */
    int inline_state;
};

typedef struct Field {
    const char *name;
    Type *type;
    Loc loc;
    ClassDecl *owner_class; /* defining class (for casts), set by checker */
} Field;

/* An enum is an int at runtime; the checker keeps it a distinct type. Variants
   carry an explicit value or continue from the previous one + 1, C-style, so a
   wire protocol's numbers can be pinned. */
typedef struct EnumVariant {
    const char *name;
    Expr *init;         /* explicit `= expr`, NULL to continue from the previous */
    int64_t value;      /* filled by the checker */
    /* `= "text"`: the variant's serialized spelling. Metadata, not the value —
       the ordinal is still assigned positionally. NULL means .name()/parse()
       use `name` above. */
    const char *text;
    int textlen;
    Loc loc;
} EnumVariant;

struct EnumDecl {
    const char *name;
    Loc loc;
    EnumVariant *variants;
    int nvariants;
    Module *module;
    bool name_tbl_emitted;  /* emit: the .name() lookup is written once, on first use */
    bool find_tbl_emitted;  /* emit: same, for the parse()/has() reverse lookup */
};

struct StructDecl {
    const char *name;
    Loc loc;
    Field *fields;
    int nfields;
    FnDecl **methods;       /* value-receiver methods */
    int nmethods;
    Module *module;
    bool is_extern;         /* extern "C" struct: emit verbatim name */
    bool checked;
    /* generics (mirror of FnDecl's set) */
    const char **typarams;
    int ntyparams;
    struct StructDecl *generic_origin;
    Type **type_args;
    struct StructDecl *next_inst;
    Loc first_use;
    bool sig_resolved;
    const char *cname;      /* memoized mangled name */
};

struct ClassDecl {
    const char *name;
    Loc loc;
    const char *parent_name;
    ClassDecl *parent;      /* resolved */
    Field *fields;
    int nfields;
    FnDecl **methods;
    int nmethods;
    /* `const NAME: T = expr;` in a class body — namespaced constants, reached
       only as Type.NAME. Folded exactly like a module const. */
    Decl **consts;
    int nconsts;
    FnDecl *ctor;           /* init */
    Stmt **deinit_body;
    int ndeinit;
    Module *module;
    int nvslots;            /* total vtable slots incl. inherited */
    bool checked;
    /* generics (mirror of FnDecl's set) */
    const char **typarams;
    int ntyparams;
    struct ClassDecl *generic_origin;
    Type **type_args;
    struct ClassDecl *next_inst;
    Loc first_use;
    bool sig_resolved;
    const char *cname;      /* memoized mangled name */
};

typedef enum DeclKind {
    D_FN, D_STRUCT, D_CLASS, D_ENUM, D_CONST, D_IMPORT, D_EXTERN_FN, D_LINK,
} DeclKind;

struct Decl {
    DeclKind kind;
    Loc loc;
    const char *name;
    Module *module;         /* owning module */
    FnDecl *fn;             /* D_FN, D_EXTERN_FN */
    StructDecl *sd;         /* D_STRUCT */
    ClassDecl *cd;          /* D_CLASS */
    EnumDecl *ed;           /* D_ENUM */
    Type *const_type;       /* D_CONST */
    Expr *const_init;
    int fold_state;         /* D_CONST folding: 0 none, 1 in-progress, 2 done */
    /* import */
    const char **import_names;
    int nimport_names;
    const char *import_path;
    Module *import_module;  /* resolved */
    const char *link_lib;   /* D_LINK */
    const char *link_cond;  /* D_LINK: OS prefix of the target triple, or NULL */
    bool wrapper_emitted;   /* D_FN used as closure value: thunk emitted */
};

/* ---------------- module ---------------- */

struct Module {
    const char *name;       /* mangled-safe module name, e.g. "main" */
    const char *path;       /* source path */
    const char *src;
    Decl **decls;
    int ndecls;
    Module *next;           /* all loaded modules */
    bool checked;
    bool resolving;         /* re-export cycle guard */
    int arrow_counter;
    uint64_t src_hash;
};

#endif
