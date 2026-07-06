#ifndef VOLT_AST_H
#define VOLT_AST_H

#include "util.h"

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct Module Module;
typedef struct FnDecl FnDecl;
typedef struct ClassDecl ClassDecl;
typedef struct StructDecl StructDecl;

/* ---------------- types ---------------- */

typedef enum TypeKind {
    TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_BYTE,
    TY_I8, TY_I16, TY_I32, TY_I64, TY_U8, TY_U16, TY_U32, TY_U64, TY_F32, TY_F64,
    TY_STRING, TY_CSTRING, TY_RAWPTR, TY_NULL,
    TY_NAMED,   /* unresolved identifier, resolved by checker */
    TY_STRUCT,  /* ->sdecl */
    TY_CLASS,   /* ->cdecl; ->weak flag on reference site */
    TY_ARRAY,   /* ->elem */
    TY_MAP,     /* ->elem = value type (keys always string) */
    TY_FN,      /* ->params/->nparams, ->ret */
} TypeKind;

struct Type {
    TypeKind kind;
    bool weak;
    Type *elem;         /* array elem / map value */
    Type *ret;          /* fn return */
    Type **params;      /* fn params */
    int nparams;
    const char *name;   /* TY_NAMED */
    Loc loc;
    StructDecl *sdecl;
    ClassDecl *cdecl;
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
    EX_AS,       /* lhs as cast_type */
    EX_STRCONV,  /* checker-inserted: to-string of lhs */
} ExprKind;

typedef enum {
    /* resolved meaning of an EX_IDENT / EX_MEMBER / EX_CALL, set by checker */
    REF_NONE, REF_LOCAL, REF_PARAM, REF_GLOBAL_FN, REF_CONST, REF_FIELD,
    REF_METHOD, REF_BUILTIN, REF_CAPTURE, REF_EXTERN_FN,
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
    FnDecl *method;         /* REF_METHOD target */
    int builtin;            /* BuiltinKind for REF_BUILTIN */
    bool is_super_call;     /* super.init(...) */
    FnDecl *fn_lit;         /* EX_ARROW body as a synthetic FnDecl */
};

typedef enum BuiltinKind {
    B_NONE, B_PRINT, B_PANIC, B_STR,
    B_LEN,          /* .len on string/array/map */
    B_PUSH, B_POP,  /* array */
    B_MAP_SET, B_MAP_GET, B_MAP_HAS, B_MAP_REMOVE,
    B_CSTR,         /* string.cstr() */
    B_SLICE,        /* string.slice(lo, hi) */
    B_CTHUNK,       /* cthunk(closure): C callback ptr, userdata first */
    B_CTHUNK_LAST,  /* cthunk_last(closure): userdata last */
    B_READFILE,     /* readfile(path): string */
    B_READLINES,    /* readlines(path): string[] */
    B_WRITEFILE,    /* writefile(path, data): bool */
    B_APPENDFILE,   /* appendfile(path, data): bool */
    B_BYTES,        /* bytes(n): byte[] — zeroed, sized buffer for FFI */
} BuiltinKind;

/* ---------------- statements ---------------- */

typedef enum StmtKind {
    ST_LET, ST_EXPR, ST_IF, ST_WHILE, ST_FOR_RANGE, ST_FOR_EACH,
    ST_RETURN, ST_BREAK, ST_CONTINUE, ST_BLOCK,
} StmtKind;

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
    Expr *iter;                /* ST_FOR_EACH: array expr */
};

/* ---------------- declarations ---------------- */

typedef struct Param {
    const char *name;
    Type *type;
    Loc loc;
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
    bool is_virtual, is_override, is_extern;
    ClassDecl *owner;       /* method owner, NULL for free fn */
    Module *module;
    int vslot;              /* vtable slot if virtual, else -1 */
    /* checker outputs */
    Local *locals;          /* linked list of all locals incl. params */
    int ntemps;
    /* closures */
    Capture *captures;
    int ncaptures;
    int arrow_id;           /* unique id for arrows within module */
    struct FnDecl *parent_fn; /* lexically enclosing fn, for arrows */
};

typedef struct Field {
    const char *name;
    Type *type;
    Loc loc;
    ClassDecl *owner_class; /* defining class (for casts), set by checker */
} Field;

struct StructDecl {
    const char *name;
    Loc loc;
    Field *fields;
    int nfields;
    Module *module;
    bool is_extern;         /* extern "C" struct: emit verbatim name */
    bool checked;
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
    FnDecl *ctor;           /* init */
    Stmt **deinit_body;
    int ndeinit;
    Module *module;
    int nvslots;            /* total vtable slots incl. inherited */
    bool checked;
};

typedef enum DeclKind {
    D_FN, D_STRUCT, D_CLASS, D_CONST, D_IMPORT, D_EXTERN_FN, D_LINK,
} DeclKind;

struct Decl {
    DeclKind kind;
    Loc loc;
    const char *name;
    Module *module;         /* owning module */
    FnDecl *fn;             /* D_FN, D_EXTERN_FN */
    StructDecl *sd;         /* D_STRUCT */
    ClassDecl *cd;          /* D_CLASS */
    Type *const_type;       /* D_CONST */
    Expr *const_init;
    /* import */
    const char **import_names;
    int nimport_names;
    const char *import_path;
    Module *import_module;  /* resolved */
    const char *link_lib;   /* D_LINK */
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
    int arrow_counter;
    uint64_t src_hash;
};

#endif
