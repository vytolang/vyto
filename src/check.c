#include "check.h"
#include "lex.h"

/* ---------------- type helpers ---------------- */

static Type *mk_type(TypeKind k) {
    Type *t = NEW(Type);
    t->kind = k;
    return t;
}

static Type *ty_void_s, *ty_int_s, *ty_float_s, *ty_bool_s, *ty_string_s, *ty_cstring_s;
static Type *ty_void(void) { return ty_void_s ? ty_void_s : (ty_void_s = mk_type(TY_VOID)); }
static Type *ty_int(void) { return ty_int_s ? ty_int_s : (ty_int_s = mk_type(TY_INT)); }
static Type *ty_float(void) { return ty_float_s ? ty_float_s : (ty_float_s = mk_type(TY_FLOAT)); }
static Type *ty_bool(void) { return ty_bool_s ? ty_bool_s : (ty_bool_s = mk_type(TY_BOOL)); }
static Type *ty_string(void) { return ty_string_s ? ty_string_s : (ty_string_s = mk_type(TY_STRING)); }
static Type *ty_cstring(void) { return ty_cstring_s ? ty_cstring_s : (ty_cstring_s = mk_type(TY_CSTRING)); }

static TypeKind norm_kind(TypeKind k) {
    if (k == TY_I64) return TY_INT;
    if (k == TY_F64) return TY_FLOAT;
    if (k == TY_U8) return TY_BYTE;
    return k;
}

bool type_identical(const Type *a, const Type *b) {
    if (norm_kind(a->kind) != norm_kind(b->kind)) return false;
    switch (a->kind) {
    case TY_STRUCT: return a->sdecl == b->sdecl;
    case TY_CLASS: return a->cdecl == b->cdecl;
    case TY_ARRAY: case TY_MAP: return type_identical(a->elem, b->elem);
    case TY_FN: {
        if (a->nparams != b->nparams) return false;
        if (!type_identical(a->ret, b->ret)) return false;
        for (int i = 0; i < a->nparams; i++)
            if (!type_identical(a->params[i], b->params[i])) return false;
        return true;
    }
    default: return true;
    }
}

static const char *type_str(const Type *t) {
    switch (t->kind) {
    case TY_VOID: return "void"; case TY_INT: return "int"; case TY_FLOAT: return "float";
    case TY_BOOL: return "bool"; case TY_BYTE: return "byte"; case TY_STRING: return "string";
    case TY_CSTRING: return "cstring"; case TY_RAWPTR: return "rawptr"; case TY_NULL: return "null";
    case TY_I8: return "i8"; case TY_I16: return "i16"; case TY_I32: return "i32"; case TY_I64: return "i64";
    case TY_U8: return "u8"; case TY_U16: return "u16"; case TY_U32: return "u32"; case TY_U64: return "u64";
    case TY_CLONG: return "clong"; case TY_CULONG: return "culong";
    case TY_F32: return "f32"; case TY_F64: return "f64";
    case TY_STRUCT: return t->sdecl->name;
    case TY_CLASS: return t->cdecl->name;
    case TY_ARRAY: return arena_printf(&g_arena, "%s[]", type_str(t->elem));
    case TY_MAP: return arena_printf(&g_arena, "Map<string, %s>", type_str(t->elem));
    case TY_FN: return "fn(...)";
    case TY_NAMED: return t->name;
    }
    return "?";
}

/* ---- integer width/sign rules for implicit widening ---- */

static int int_width(TypeKind k) {
    switch (norm_kind(k)) {
    case TY_BYTE: case TY_I8: return 1;
    case TY_I16: case TY_U16: return 2;
    case TY_I32: case TY_U32: return 4;
    default: return 8; /* TY_INT, TY_U64, TY_CLONG, TY_CULONG (LP64 model) */
    }
}

static bool int_is_signed(TypeKind k) {
    switch (norm_kind(k)) {
    case TY_BYTE: case TY_U16: case TY_U32: case TY_U64: case TY_CULONG: return false;
    default: return true;
    }
}

/* value-preserving implicit conversion: src fits in dst for every value */
static bool int_widens(const Type *src, const Type *dst) {
    if (!type_is_int(src) || !type_is_int(dst)) return false;
    int sw = int_width(src->kind), dw = int_width(dst->kind);
    bool ss = int_is_signed(src->kind), ds = int_is_signed(dst->kind);
    if (ss == ds) return dw > sw;
    if (!ss && ds) return dw > sw; /* unsigned fits in a strictly wider signed */
    return false;
}

/* common type of two numeric operands, or NULL if they don't mix implicitly */
static Type *num_join(Type *lt, Type *rt) {
    if (type_identical(lt, rt)) return lt;
    if (int_widens(lt, rt)) return rt;
    if (int_widens(rt, lt)) return lt;
    return NULL;
}

bool class_derives(const ClassDecl *sub, const ClassDecl *base) {
    for (const ClassDecl *c = sub; c; c = c->parent)
        if (c == base) return true;
    return false;
}

ClassDecl *class_find_field(ClassDecl *cd, const char *name, Field **out) {
    for (ClassDecl *c = cd; c; c = c->parent)
        for (int i = 0; i < c->nfields; i++)
            if (c->fields[i].name == name) {
                if (out) *out = &c->fields[i];
                return c;
            }
    return NULL;
}

FnDecl *class_find_method(ClassDecl *cd, const char *name) {
    for (ClassDecl *c = cd; c; c = c->parent)
        for (int i = 0; i < c->nmethods; i++)
            if (c->methods[i]->name == name) return c->methods[i];
    return NULL;
}

/* ---------------- module symbols ---------------- */

static Decl *mod_lookup_own(Module *m, const char *name) {
    for (int i = 0; i < m->ndecls; i++) {
        Decl *d = m->decls[i];
        switch (d->kind) {
        case D_FN: case D_STRUCT: case D_CLASS: case D_CONST: case D_EXTERN_FN:
            if (d->name == name) return d;
            break;
        default: break;
        }
    }
    return NULL;
}

static Decl *mod_lookup(Module *m, const char *name) {
    Decl *d = mod_lookup_own(m, name);
    if (d) return d;
    for (int i = 0; i < m->ndecls; i++) {
        Decl *imp = m->decls[i];
        if (imp->kind != D_IMPORT) continue;
        for (int j = 0; j < imp->nimport_names; j++) {
            if (imp->import_names[j] == name)
                return mod_lookup_own(imp->import_module, name);
        }
    }
    return NULL;
}

/* ---------------- type resolution ---------------- */

static void resolve_type(Type *t, Module *m) {
    if (!t) return;
    switch (t->kind) {
    case TY_NAMED: {
        Decl *d = mod_lookup(m, t->name);
        if (!d) fatal_at(t->loc, "unknown type '%s'", t->name);
        if (d->kind == D_STRUCT) { t->kind = TY_STRUCT; t->sdecl = d->sd; }
        else if (d->kind == D_CLASS) { t->kind = TY_CLASS; t->cdecl = d->cd; }
        else fatal_at(t->loc, "'%s' is not a type", t->name);
        break;
    }
    case TY_ARRAY: case TY_MAP:
        resolve_type(t->elem, m);
        break;
    case TY_FN:
        resolve_type(t->ret, m);
        for (int i = 0; i < t->nparams; i++) resolve_type(t->params[i], m);
        break;
    default: break;
    }
    if (t->weak && t->kind != TY_CLASS)
        fatal_at(t->loc, "'weak' applies only to class types");
}

static void resolve_fn_sig(FnDecl *fd, Module *m) {
    for (int i = 0; i < fd->nparams; i++) resolve_type(fd->params[i].type, m);
    resolve_type(fd->ret, m);
}

static bool valid_extern_field(const Type *t) {
    switch (t->kind) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE:
    case TY_I8: case TY_I16: case TY_I32: case TY_I64:
    case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_F32: case TY_F64:
    case TY_CLONG: case TY_CULONG:
    case TY_CSTRING: case TY_RAWPTR:
        return true;
    case TY_STRUCT: return t->sdecl->is_extern;
    default: return false;
    }
}

static void resolve_module_sigs(Module *m) {
    for (int i = 0; i < m->ndecls; i++) {
        Decl *d = m->decls[i];
        switch (d->kind) {
        case D_FN: case D_EXTERN_FN:
            resolve_fn_sig(d->fn, m);
            break;
        case D_CONST:
            resolve_type(d->const_type, m);
            break;
        case D_STRUCT: {
            StructDecl *sd = d->sd;
            for (int f = 0; f < sd->nfields; f++) {
                resolve_type(sd->fields[f].type, m);
                Type *ft = sd->fields[f].type;
                if (sd->is_extern) {
                    if (!valid_extern_field(ft))
                        fatal_at(sd->fields[f].loc, "extern struct field must have a C-compatible type");
                } else if (type_is_ref(ft) || ft->kind == TY_VOID) {
                    fatal_at(sd->fields[f].loc,
                             "struct fields must be value types in v0.1 (use a class for '%s')",
                             sd->fields[f].name);
                }
            }
            break;
        }
        case D_CLASS: {
            ClassDecl *cd = d->cd;
            if (cd->parent_name) {
                Decl *pd = mod_lookup(m, cd->parent_name);
                if (!pd || pd->kind != D_CLASS)
                    fatal_at(cd->loc, "unknown base class '%s'", cd->parent_name);
                cd->parent = pd->cd;
            }
            for (int f = 0; f < cd->nfields; f++) {
                resolve_type(cd->fields[f].type, m);
                if (cd->fields[f].type->kind == TY_VOID)
                    fatal_at(cd->fields[f].loc, "field cannot be void");
                cd->fields[f].owner_class = cd;
            }
            for (int mi = 0; mi < cd->nmethods; mi++) resolve_fn_sig(cd->methods[mi], m);
            if (cd->ctor) resolve_fn_sig(cd->ctor, m);
            break;
        }
        default: break;
        }
    }
}

/* ---------------- class layout (vtable slots) ---------------- */

static bool fn_sig_identical(FnDecl *a, FnDecl *b) {
    if (a->nparams != b->nparams) return false;
    if (!type_identical(a->ret, b->ret)) return false;
    for (int i = 0; i < a->nparams; i++)
        if (!type_identical(a->params[i].type, b->params[i].type)) return false;
    return true;
}

static void layout_class(ClassDecl *cd) {
    if (cd->checked) return;
    cd->checked = true;
    if (cd->parent) {
        if (class_derives(cd->parent, cd)) fatal_at(cd->loc, "inheritance cycle");
        layout_class(cd->parent);
        cd->nvslots = cd->parent->nvslots;
        for (int i = 0; i < cd->nfields; i++) {
            Field *shadow;
            if (class_find_field(cd->parent, cd->fields[i].name, &shadow))
                fatal_at(cd->fields[i].loc, "field '%s' shadows inherited field", cd->fields[i].name);
        }
    }
    for (int i = 0; i < cd->nmethods; i++) {
        FnDecl *m = cd->methods[i];
        FnDecl *inherited = cd->parent ? class_find_method(cd->parent, m->name) : NULL;
        if (m->is_override) {
            if (!inherited || !(inherited->is_virtual || inherited->is_override))
                fatal_at(m->loc, "'%s' overrides nothing virtual", m->name);
            if (!fn_sig_identical(m, inherited))
                fatal_at(m->loc, "override '%s' has a different signature", m->name);
            m->vslot = inherited->vslot;
        } else if (m->is_virtual) {
            if (inherited) fatal_at(m->loc, "'%s' hides inherited method; use 'override'", m->name);
            m->vslot = cd->nvslots++;
        } else {
            if (inherited) fatal_at(m->loc, "'%s' hides inherited method", m->name);
        }
        /* duplicate names within the class */
        for (int j = 0; j < i; j++)
            if (cd->methods[j]->name == m->name) fatal_at(m->loc, "duplicate method '%s'", m->name);
        if (class_find_field(cd, m->name, NULL))
            fatal_at(m->loc, "method '%s' collides with a field", m->name);
    }
}

/* ---------------- scopes ---------------- */

typedef struct ScopeEntry {
    const char *name;
    Local *local;
    struct ScopeEntry *next;
} ScopeEntry;

typedef struct Scope {
    ScopeEntry *entries;
    struct Scope *parent;
} Scope;

typedef struct Ctx {
    Module *mod;
    FnDecl *fn;          /* function whose body is being checked */
    ClassDecl *cls;      /* enclosing class for methods */
    Scope *scope;
    Scope *outer_scope;  /* for arrows: enclosing function's scope chain */
    struct Ctx *outer;   /* enclosing fn ctx (arrows) */
    int loop_depth;
    int local_counter;
} Ctx;

static Scope *scope_push(Ctx *c) {
    Scope *s = NEW(Scope);
    s->parent = c->scope;
    c->scope = s;
    return s;
}

static void scope_pop(Ctx *c) { c->scope = c->scope->parent; }

static Local *scope_find(Scope *s, const char *name) {
    for (; s; s = s->parent)
        for (ScopeEntry *e = s->entries; e; e = e->next)
            if (e->name == name) return e->local;
    return NULL;
}

static Local *define_local(Ctx *c, const char *name, Type *type, bool is_param, Loc loc) {
    for (ScopeEntry *e = c->scope->entries; e; e = e->next)
        if (e->name == name) fatal_at(loc, "duplicate name '%s' in this scope", name);
    Local *l = NEW(Local);
    l->name = name;
    l->type = type;
    l->is_param = is_param;
    l->cname = is_param ? arena_printf(&g_arena, "p_%s", name)
                        : arena_printf(&g_arena, "l_%s_%d", name, ++c->local_counter);
    l->next_in_fn = c->fn->locals;
    c->fn->locals = l;
    ScopeEntry *e = NEW(ScopeEntry);
    e->name = name;
    e->local = l;
    e->next = c->scope->entries;
    c->scope->entries = e;
    return l;
}

/* ---------------- expressions ---------------- */

static Type *check_expr(Ctx *c, Expr *e, Type *expected);
static void check_block(Ctx *c, Stmt **body, int n);

static Expr *strconv(Expr *e) {
    if (e->type->kind == TY_STRING) return e;
    Expr *w = NEW(Expr);
    w->kind = EX_STRCONV;
    w->loc = e->loc;
    w->lhs = e;
    w->type = ty_string();
    return w;
}

static bool str_convertible(const Type *t) {
    return t->kind == TY_STRING || type_is_num(t) || t->kind == TY_BOOL;
}

/* types that can cross the C ABI boundary in a callback signature */
static bool ffi_safe_type(const Type *t) {
    return type_is_num(t) || t->kind == TY_BOOL || t->kind == TY_CSTRING ||
           t->kind == TY_RAWPTR;
}

/* can `src` (with expr e, may be NULL) be assigned to dst? adapts literals. */
static bool assignable(Type *dst, Type *src, Expr *e) {
    if (type_identical(dst, src)) return true;
    if (src->kind == TY_NULL &&
        ((type_is_ref(dst) && dst->kind != TY_NULL) || dst->kind == TY_RAWPTR ||
         dst->kind == TY_CSTRING)) {
        if (e) e->type = dst;
        return true;
    }
    if (e && e->kind == EX_INT && type_is_num(dst)) { e->type = dst; return true; }
    if (e && e->kind == EX_FLOAT && type_is_float(dst)) { e->type = dst; return true; }
    if (e && e->kind == EX_UN && e->op == T_MINUS && e->lhs &&
        (e->lhs->kind == EX_INT || e->lhs->kind == EX_FLOAT)) {
        if ((e->lhs->kind == EX_INT && type_is_num(dst)) ||
            (e->lhs->kind == EX_FLOAT && type_is_float(dst))) {
            e->type = e->lhs->type = dst;
            return true;
        }
    }
    if (dst->kind == TY_CLASS && src->kind == TY_CLASS && class_derives(src->cdecl, dst->cdecl))
        return true; /* implicit upcast */
    if (int_widens(src, dst)) return true; /* value-preserving integer widening */
    return false;
}

/* An integer-literal-only constant expression — a literal, a negated one, or
   arithmetic (+ - * /) over such. These are "untyped" and may fold to float in
   a float context (Go-style), at compile time with no runtime conversion. `%`
   is excluded because it has no float form. */
static bool is_int_const_expr(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: return true;
    case EX_UN: return e->op == T_MINUS && is_int_const_expr(e->lhs);
    case EX_BIN:
        switch (e->op) {
        case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH:
            return is_int_const_expr(e->lhs) && is_int_const_expr(e->rhs);
        default: return false;
        }
    default: return false;
    }
}

/* Retype an int-constant subtree to float f; the emitter then folds its
   literals to C double constants (and `/` becomes float division). */
static void fold_to_float(Expr *e, Type *f) {
    e->type = f;
    if (e->kind == EX_UN) fold_to_float(e->lhs, f);
    else if (e->kind == EX_BIN) { fold_to_float(e->lhs, f); fold_to_float(e->rhs, f); }
}

static void want(Ctx *c, Expr *e, Type *dst, const char *what) {
    (void)c;
    if (!assignable(dst, e->type, e))
        fatal_at(e->loc, "type mismatch in %s: expected %s, got %s", what,
                 type_str(dst), type_str(e->type));
}

static void check_args_against(Ctx *c, Expr *e, Param *params, int nparams, const char *name) {
    if (e->nargs != nparams)
        fatal_at(e->loc, "'%s' expects %d argument(s), got %d", name, nparams, e->nargs);
    for (int i = 0; i < nparams; i++) {
        check_expr(c, e->args[i], params[i].type);
        want(c, e->args[i], params[i].type, "argument");
    }
}

static Type *fn_type_of(FnDecl *fd) {
    Type *t = mk_type(TY_FN);
    t->nparams = fd->nparams;
    t->params = arena_alloc(&g_arena, sizeof(Type *) * (size_t)(fd->nparams ? fd->nparams : 1));
    for (int i = 0; i < fd->nparams; i++) t->params[i] = fd->params[i].type;
    t->ret = fd->ret;
    return t;
}

static Local *lookup_value(Ctx *c, Expr *e, const char *name) {
    Local *l = scope_find(c->scope, name);
    if (l) {
        e->ref = l->is_param ? REF_PARAM : REF_LOCAL;
        e->local = l;
        return l;
    }
    /* capture from enclosing function (arrows only, one level) */
    if (c->outer_scope) {
        l = scope_find(c->outer_scope, name);
        if (l) {
            if (l->is_this) fatal_at(e->loc, "closures cannot capture 'this' in v0.1");
            FnDecl *fd = c->fn;
            for (int i = 0; i < fd->ncaptures; i++)
                if (fd->captures[i].src == l) { e->ref = REF_CAPTURE; e->local = l; return l; }
            Capture *nc = arena_alloc(&g_arena, sizeof(Capture) * (size_t)(fd->ncaptures + 1));
            memcpy(nc, fd->captures, sizeof(Capture) * (size_t)fd->ncaptures);
            nc[fd->ncaptures].src = l;
            nc[fd->ncaptures].name = name;
            nc[fd->ncaptures].type = l->type;
            fd->captures = nc;
            fd->ncaptures++;
            e->ref = REF_CAPTURE;
            e->local = l;
            return l;
        }
    }
    return NULL;
}

static void check_closure_call(Ctx *c, Expr *e, Type *fnty) {
    if (e->nargs != fnty->nparams)
        fatal_at(e->loc, "closure expects %d argument(s), got %d", fnty->nparams, e->nargs);
    for (int i = 0; i < fnty->nparams; i++) {
        check_expr(c, e->args[i], fnty->params[i]);
        want(c, e->args[i], fnty->params[i], "argument");
    }
    e->ref = REF_NONE;
    e->type = fnty->ret;
}

static Type *check_call(Ctx *c, Expr *e) {
    if (e->is_super_call) {
        if (!c->cls || !c->fn || c->fn != c->cls->ctor)
            fatal_at(e->loc, "super.init() is only allowed inside init");
        if (!c->cls->parent) fatal_at(e->loc, "class has no base class");
        FnDecl *pc = c->cls->parent->ctor;
        e->cls = c->cls->parent;
        if (!pc) {
            if (e->nargs != 0) fatal_at(e->loc, "base class has no init taking arguments");
        } else {
            check_args_against(c, e, pc->params, pc->nparams, "super.init");
        }
        return e->type = ty_void();
    }

    Expr *callee = e->lhs;

    if (callee->kind == EX_IDENT) {
        const char *n = callee->name;
        if (n == intern("print") || n == intern("panic")) {
            if (e->nargs != 1) fatal_at(e->loc, "%s takes 1 argument", n);
            check_expr(c, e->args[0], ty_string());
            if (!str_convertible(e->args[0]->type))
                fatal_at(e->args[0]->loc, "%s expects a string", n);
            e->args[0] = strconv(e->args[0]);
            e->ref = REF_BUILTIN;
            e->builtin = n == intern("print") ? B_PRINT : B_PANIC;
            return e->type = ty_void();
        }
        if (n == intern("cthunk") || n == intern("cthunk_last")) {
            if (e->nargs != 1) fatal_at(e->loc, "%s takes 1 argument", n);
            Type *t = check_expr(c, e->args[0], NULL);
            if (t->kind != TY_FN) fatal_at(e->args[0]->loc, "%s expects a closure", n);
            for (int i = 0; i < t->nparams; i++)
                if (!ffi_safe_type(t->params[i]))
                    fatal_at(e->args[0]->loc,
                             "closure parameter %d is not C-compatible (use numeric, cstring, or rawptr types)",
                             i + 1);
            if (t->ret->kind != TY_VOID && !ffi_safe_type(t->ret))
                fatal_at(e->args[0]->loc, "closure return type is not C-compatible");
            e->ref = REF_BUILTIN;
            e->builtin = n == intern("cthunk") ? B_CTHUNK : B_CTHUNK_LAST;
            Type *r = mk_type(TY_RAWPTR);
            return e->type = r;
        }
        if (n == intern("str")) {
            if (e->nargs != 1) fatal_at(e->loc, "str takes 1 argument");
            check_expr(c, e->args[0], NULL);
            if (!str_convertible(e->args[0]->type) && e->args[0]->type->kind != TY_CSTRING)
                fatal_at(e->args[0]->loc, "str() expects int, float, bool, or cstring");
            e->ref = REF_BUILTIN;
            e->builtin = B_STR;
            return e->type = ty_string();
        }
        if (n == intern("readfile") || n == intern("readlines")) {
            if (e->nargs != 1) fatal_at(e->loc, "%s takes 1 argument", n);
            check_expr(c, e->args[0], ty_string());
            want(c, e->args[0], ty_string(), "path");
            e->ref = REF_BUILTIN;
            if (n == intern("readfile")) { e->builtin = B_READFILE; return e->type = ty_string(); }
            e->builtin = B_READLINES;
            Type *t = mk_type(TY_ARRAY);
            t->elem = ty_string();
            return e->type = t;
        }
        if (n == intern("listdir")) {
            if (e->nargs != 1) fatal_at(e->loc, "listdir takes 1 argument");
            check_expr(c, e->args[0], ty_string());
            want(c, e->args[0], ty_string(), "path");
            e->ref = REF_BUILTIN;
            e->builtin = B_LISTDIR;
            Type *t = mk_type(TY_ARRAY);
            t->elem = ty_string();
            return e->type = t;
        }
        if (n == intern("isdir")) {
            if (e->nargs != 1) fatal_at(e->loc, "isdir takes 1 argument");
            check_expr(c, e->args[0], ty_string());
            want(c, e->args[0], ty_string(), "path");
            e->ref = REF_BUILTIN;
            e->builtin = B_ISDIR;
            return e->type = ty_bool();
        }
        if (n == intern("writefile") || n == intern("appendfile")) {
            if (e->nargs != 2) fatal_at(e->loc, "%s takes 2 arguments (path, data)", n);
            for (int i = 0; i < 2; i++) {
                check_expr(c, e->args[i], ty_string());
                want(c, e->args[i], ty_string(), i == 0 ? "path" : "data");
            }
            e->ref = REF_BUILTIN;
            e->builtin = n == intern("writefile") ? B_WRITEFILE : B_APPENDFILE;
            return e->type = ty_bool();
        }
        if (n == intern("bytes")) {
            if (e->nargs != 1) fatal_at(e->loc, "bytes takes 1 argument");
            check_expr(c, e->args[0], NULL);
            want(c, e->args[0], ty_int(), "size");
            e->ref = REF_BUILTIN;
            e->builtin = B_BYTES;
            Type *t = mk_type(TY_ARRAY);
            t->elem = mk_type(TY_BYTE);
            return e->type = t;
        }
        /* local closure variable? */
        Local *l = lookup_value(c, callee, n);
        if (l) {
            if (l->type->kind != TY_FN)
                fatal_at(e->loc, "'%s' is not callable", n);
            callee->type = l->type;
            check_closure_call(c, e, l->type);
            return e->type;
        }
        Decl *d = mod_lookup(c->mod, n);
        if (!d) fatal_at(e->loc, "unknown function '%s'", n);
        switch (d->kind) {
        case D_FN:
            e->ref = REF_GLOBAL_FN;
            e->decl = d;
            check_args_against(c, e, d->fn->params, d->fn->nparams, n);
            return e->type = d->fn->ret;
        case D_EXTERN_FN:
            e->ref = REF_EXTERN_FN;
            e->decl = d;
            check_args_against(c, e, d->fn->params, d->fn->nparams, n);
            return e->type = d->fn->ret;
        case D_STRUCT: {
            StructDecl *sd = d->sd;
            if (e->nargs != 0 && e->nargs != sd->nfields)
                fatal_at(e->loc, "struct %s takes 0 or %d arguments", n, sd->nfields);
            for (int i = 0; i < e->nargs; i++) {
                check_expr(c, e->args[i], sd->fields[i].type);
                want(c, e->args[i], sd->fields[i].type, "struct field");
            }
            e->sd = sd;
            Type *t = mk_type(TY_STRUCT);
            t->sdecl = sd;
            return e->type = t;
        }
        case D_CLASS:
            fatal_at(e->loc, "use 'new %s(...)' to create a class instance", n);
        default:
            fatal_at(e->loc, "'%s' is not callable", n);
        }
    }

    if (callee->kind == EX_MEMBER) {
        Type *recv = check_expr(c, callee->lhs, NULL);
        const char *n = callee->name;
        if (recv->kind == TY_ARRAY) {
            if (n == intern("push")) {
                if (e->nargs != 1) fatal_at(e->loc, "push takes 1 argument");
                check_expr(c, e->args[0], recv->elem);
                want(c, e->args[0], recv->elem, "push");
                e->ref = REF_BUILTIN; e->builtin = B_PUSH;
                return e->type = ty_void();
            }
            if (n == intern("pop")) {
                if (e->nargs != 0) fatal_at(e->loc, "pop takes no arguments");
                e->ref = REF_BUILTIN; e->builtin = B_POP;
                return e->type = recv->elem;
            }
            fatal_at(e->loc, "arrays have no method '%s'", n);
        }
        if (recv->kind == TY_MAP) {
            if (n == intern("set")) {
                if (e->nargs != 2) fatal_at(e->loc, "set takes 2 arguments");
                check_expr(c, e->args[0], ty_string());
                want(c, e->args[0], ty_string(), "map key");
                check_expr(c, e->args[1], recv->elem);
                want(c, e->args[1], recv->elem, "map value");
                e->ref = REF_BUILTIN; e->builtin = B_MAP_SET;
                return e->type = ty_void();
            }
            if (n == intern("get") || n == intern("has") || n == intern("remove")) {
                if (e->nargs != 1) fatal_at(e->loc, "%s takes 1 argument", n);
                check_expr(c, e->args[0], ty_string());
                want(c, e->args[0], ty_string(), "map key");
                e->ref = REF_BUILTIN;
                if (n == intern("get")) { e->builtin = B_MAP_GET; return e->type = recv->elem; }
                if (n == intern("has")) { e->builtin = B_MAP_HAS; return e->type = ty_bool(); }
                e->builtin = B_MAP_REMOVE;
                return e->type = ty_void();
            }
            fatal_at(e->loc, "maps have no method '%s'", n);
        }
        if (recv->kind == TY_STRING) {
            if (n == intern("cstr")) {
                if (e->nargs != 0) fatal_at(e->loc, "cstr takes no arguments");
                e->ref = REF_BUILTIN; e->builtin = B_CSTR;
                return e->type = ty_cstring();
            }
            if (n == intern("slice")) {
                if (e->nargs != 2) fatal_at(e->loc, "slice takes 2 arguments");
                for (int i = 0; i < 2; i++) {
                    check_expr(c, e->args[i], NULL);
                    want(c, e->args[i], ty_int(), "slice bound");
                }
                e->ref = REF_BUILTIN; e->builtin = B_SLICE;
                return e->type = ty_string();
            }
            fatal_at(e->loc, "strings have no method '%s'", n);
        }
        if (recv->kind == TY_CLASS) {
            if (n == intern("init")) fatal_at(e->loc, "init is called via 'new' or 'super.init'");
            FnDecl *m = class_find_method(recv->cdecl, n);
            if (m) {
                e->ref = REF_METHOD;
                e->method = m;
                e->cls = m->owner;
                check_args_against(c, e, m->params, m->nparams, n);
                return e->type = m->ret;
            }
            Field *f;
            ClassDecl *owner = class_find_field(recv->cdecl, n, &f);
            if (owner && f->type->kind == TY_FN) {
                callee->ref = REF_FIELD;
                callee->cls = owner;
                callee->type = f->type;
                check_closure_call(c, e, f->type);
                return e->type;
            }
            fatal_at(e->loc, "class %s has no method '%s'", recv->cdecl->name, n);
        }
        fatal_at(e->loc, "%s has no methods", type_str(recv));
    }

    /* arbitrary closure-typed expression */
    Type *t = check_expr(c, callee, NULL);
    if (t->kind != TY_FN) fatal_at(e->loc, "expression is not callable");
    check_closure_call(c, e, t);
    return e->type;
}

static Type *check_member(Ctx *c, Expr *e) {
    Type *recv = check_expr(c, e->lhs, NULL);
    const char *n = e->name;
    if (n == intern("len") &&
        (recv->kind == TY_STRING || recv->kind == TY_ARRAY || recv->kind == TY_MAP)) {
        e->ref = REF_BUILTIN;
        e->builtin = B_LEN;
        return e->type = ty_int();
    }
    if (recv->kind == TY_STRUCT) {
        StructDecl *sd = recv->sdecl;
        for (int i = 0; i < sd->nfields; i++)
            if (sd->fields[i].name == n) {
                e->ref = REF_FIELD;
                e->sd = sd;
                return e->type = sd->fields[i].type;
            }
        fatal_at(e->loc, "struct %s has no field '%s'", sd->name, n);
    }
    if (recv->kind == TY_CLASS) {
        Field *f;
        ClassDecl *owner = class_find_field(recv->cdecl, n, &f);
        if (!owner) fatal_at(e->loc, "class %s has no field '%s'", recv->cdecl->name, n);
        e->ref = REF_FIELD;
        e->cls = owner;
        Type *t = f->type;
        if (t->weak) { /* loading a weak field yields a normal (borrowed) reference */
            Type *s = NEW(Type);
            *s = *t;
            s->weak = false;
            t = s;
        }
        return e->type = t;
    }
    fatal_at(e->loc, "%s has no member '%s'", type_str(recv), n);
    return NULL;
}

static void check_arrow(Ctx *c, Expr *e, Type *expected) {
    if (!expected || expected->kind != TY_FN)
        fatal_at(e->loc, "cannot infer closure type here; assign to a typed target");
    FnDecl *fd = e->fn_lit;
    if (fd->nparams != expected->nparams)
        fatal_at(e->loc, "closure has %d parameter(s), expected %d", fd->nparams, expected->nparams);
    for (int i = 0; i < fd->nparams; i++) fd->params[i].type = expected->params[i];
    fd->ret = expected->ret;
    fd->parent_fn = c->fn;
    if (c->outer_scope) fatal_at(e->loc, "nested closures are not supported in v0.1");

    Ctx ac = {0};
    ac.mod = c->mod;
    ac.fn = fd;
    ac.cls = NULL;
    ac.outer_scope = c->scope;
    ac.outer = c;
    scope_push(&ac);
    for (int i = 0; i < fd->nparams; i++)
        fd->params[i].local = define_local(&ac, fd->params[i].name, fd->params[i].type, true,
                                           fd->params[i].loc);
    /* expression-sugar body returning into a void closure becomes a plain statement */
    if (fd->ret->kind == TY_VOID && fd->nbody == 1 && fd->body[0]->kind == ST_RETURN &&
        fd->body[0]->expr)
        fd->body[0]->kind = ST_EXPR;
    check_block(&ac, fd->body, fd->nbody);
    scope_pop(&ac);
    e->type = expected;
}

static Type *check_expr(Ctx *c, Expr *e, Type *expected) {
    switch (e->kind) {
    case EX_INT:
        if (expected && type_is_num(expected)) return e->type = expected;
        return e->type = ty_int();
    case EX_FLOAT:
        if (expected && type_is_float(expected)) return e->type = expected;
        return e->type = ty_float();
    case EX_STR: return e->type = ty_string();
    case EX_BOOL: return e->type = ty_bool();
    case EX_NULL:
        if (expected && (type_is_ref(expected) || expected->kind == TY_RAWPTR ||
                         expected->kind == TY_CSTRING))
            return e->type = expected;
        e->type = mk_type(TY_NULL);
        return e->type;
    case EX_THIS: {
        if (!c->cls) fatal_at(e->loc, "'this' outside a class method");
        Local *l = scope_find(c->scope, intern("this"));
        e->ref = REF_PARAM;
        e->local = l;
        return e->type = l->type;
    }
    case EX_IDENT: {
        Local *l = lookup_value(c, e, e->name);
        if (l) return e->type = l->type;
        Decl *d = mod_lookup(c->mod, e->name);
        if (!d) fatal_at(e->loc, "unknown name '%s'", e->name);
        if (d->kind == D_CONST) {
            e->ref = REF_CONST;
            e->decl = d;
            return e->type = d->const_type;
        }
        if (d->kind == D_FN) { /* function used as a closure value */
            e->ref = REF_GLOBAL_FN;
            e->decl = d;
            return e->type = fn_type_of(d->fn);
        }
        fatal_at(e->loc, "'%s' cannot be used as a value", e->name);
    }
    case EX_UN: {
        check_expr(c, e->lhs,
                   expected && (e->op == T_MINUS || e->op == T_TILDE) ? expected : NULL);
        if (e->op == T_NOT) {
            if (e->lhs->type->kind != TY_BOOL) fatal_at(e->loc, "'!' needs a bool");
            return e->type = ty_bool();
        }
        if (e->op == T_TILDE) {
            if (!type_is_int(e->lhs->type)) fatal_at(e->loc, "'~' needs an integer");
            return e->type = e->lhs->type;
        }
        if (e->op == T_AMP) {
            if (e->lhs->kind != EX_IDENT ||
                (e->lhs->ref != REF_LOCAL && e->lhs->ref != REF_PARAM))
                fatal_at(e->loc, "'&' needs a local variable or parameter");
            if (type_is_ref(e->lhs->type))
                fatal_at(e->loc, "cannot take the address of a ref-counted value (%s)",
                         type_str(e->lhs->type));
            return e->type = mk_type(TY_RAWPTR);
        }
        if (!type_is_num(e->lhs->type)) fatal_at(e->loc, "unary '-' needs a number");
        return e->type = e->lhs->type;
    }
    case EX_BIN: {
        Type *lt = check_expr(c, e->lhs, NULL);
        Type *rt = check_expr(c, e->rhs, lt->kind == TY_NULL ? NULL : lt);
        if (lt->kind == TY_NULL) { lt = check_expr(c, e->lhs, rt); }
        /* Fold an untyped integer constant to the float operand (Go-style). */
        if (type_is_float(lt) && !type_is_float(rt) && is_int_const_expr(e->rhs)) {
            fold_to_float(e->rhs, lt); rt = lt;
        } else if (type_is_float(rt) && !type_is_float(lt) && is_int_const_expr(e->lhs)) {
            fold_to_float(e->lhs, rt); lt = rt;
        }
        switch (e->op) {
        case T_PLUS:
            if (lt->kind == TY_STRING || rt->kind == TY_STRING) {
                if (!str_convertible(lt) || !str_convertible(rt))
                    fatal_at(e->loc, "cannot concatenate %s and %s", type_str(lt), type_str(rt));
                e->lhs = strconv(e->lhs);
                e->rhs = strconv(e->rhs);
                return e->type = ty_string();
            }
            /* fall through */
        case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: {
            Type *jt = type_is_num(lt) ? num_join(lt, rt) : NULL;
            if (!jt)
                fatal_at(e->loc, "arithmetic needs matching numeric types (%s vs %s)",
                         type_str(lt), type_str(rt));
            if (e->op == T_PERCENT && type_is_float(jt))
                fatal_at(e->loc, "'%%' is integer-only");
            return e->type = jt;
        }
        case T_AMP: case T_PIPE: case T_CARET: {
            Type *jt = type_is_int(lt) ? num_join(lt, rt) : NULL;
            if (!jt)
                fatal_at(e->loc, "bitwise ops need matching integer types (%s vs %s)",
                         type_str(lt), type_str(rt));
            return e->type = jt;
        }
        case T_SHL: case T_SHR:
            if (!type_is_int(lt)) fatal_at(e->loc, "shift needs an integer, got %s", type_str(lt));
            if (!type_is_int(rt)) fatal_at(e->loc, "shift amount must be an integer");
            return e->type = lt;
        case T_LT: case T_LE: case T_GT: case T_GE:
            if (!type_is_num(lt) || !num_join(lt, rt))
                fatal_at(e->loc, "comparison needs matching numeric types (%s vs %s)",
                         type_str(lt), type_str(rt));
            return e->type = ty_bool();
        case T_EQ: case T_NEQ: {
            bool ok = false;
            if (type_is_int(lt) && type_is_int(rt) && num_join(lt, rt)) ok = true;
            if (type_identical(lt, rt) &&
                (type_is_num(lt) || lt->kind == TY_BOOL || lt->kind == TY_STRING ||
                 lt->kind == TY_CLASS || lt->kind == TY_CSTRING || lt->kind == TY_RAWPTR ||
                 lt->kind == TY_FN || lt->kind == TY_ARRAY || lt->kind == TY_MAP))
                ok = true;
            if (lt->kind == TY_NULL || rt->kind == TY_NULL) {
                Type *other = lt->kind == TY_NULL ? rt : lt;
                if (type_is_ref(other) || other->kind == TY_NULL ||
                    other->kind == TY_RAWPTR || other->kind == TY_CSTRING)
                    ok = true;
            }
            if (lt->kind == TY_CLASS && rt->kind == TY_CLASS &&
                (class_derives(lt->cdecl, rt->cdecl) || class_derives(rt->cdecl, lt->cdecl)))
                ok = true;
            if (!ok) fatal_at(e->loc, "cannot compare %s and %s", type_str(lt), type_str(rt));
            return e->type = ty_bool();
        }
        case T_ANDAND: case T_OROR:
            if (lt->kind != TY_BOOL || rt->kind != TY_BOOL)
                fatal_at(e->loc, "'&&'/'||' need bools");
            return e->type = ty_bool();
        }
        fatal_at(e->loc, "bad binary operator");
    }
    case EX_ASSIGN: {
        Expr *lhs = e->lhs;
        Type *lt;
        if (lhs->kind == EX_IDENT) {
            Local *l = lookup_value(c, lhs, lhs->name);
            if (!l) fatal_at(lhs->loc, "unknown variable '%s'", lhs->name);
            if (lhs->ref == REF_CAPTURE)
                fatal_at(lhs->loc, "cannot assign to captured variable (captures are by value)");
            lt = lhs->type = l->type;
        } else if (lhs->kind == EX_MEMBER) {
            lt = check_member(c, lhs);
            if (lhs->ref != REF_FIELD) fatal_at(lhs->loc, "not assignable");
        } else if (lhs->kind == EX_INDEX) {
            Type *at = check_expr(c, lhs->lhs, NULL);
            if (at->kind != TY_ARRAY) fatal_at(lhs->loc, "only array elements are assignable");
            check_expr(c, lhs->rhs, NULL);
            want(c, lhs->rhs, ty_int(), "index");
            lt = lhs->type = at->elem;
        } else {
            fatal_at(lhs->loc, "not assignable");
            return NULL;
        }
        check_expr(c, e->rhs, lt);
        if (e->op == T_ASSIGN) {
            want(c, e->rhs, lt, "assignment");
        } else if (e->op == T_PLUSEQ && lt->kind == TY_STRING) {
            if (!str_convertible(e->rhs->type)) fatal_at(e->loc, "cannot append %s to string", type_str(e->rhs->type));
            e->rhs = strconv(e->rhs);
        } else {
            bool bitop = e->op == T_AMPEQ || e->op == T_PIPEEQ || e->op == T_CARETEQ ||
                         e->op == T_SHLEQ || e->op == T_SHREQ;
            if (bitop) {
                if (!type_is_int(lt)) fatal_at(e->loc, "bitwise assignment needs an integer");
            } else if (!type_is_num(lt)) {
                fatal_at(e->loc, "compound assignment needs numbers");
            }
            want(c, e->rhs, lt, "assignment");
            if (e->op == T_PERCENTEQ && type_is_float(lt)) fatal_at(e->loc, "'%%=' is integer-only");
        }
        return e->type = ty_void();
    }
    case EX_CALL: return check_call(c, e);
    case EX_INDEX: {
        Type *t = check_expr(c, e->lhs, NULL);
        check_expr(c, e->rhs, NULL);
        want(c, e->rhs, ty_int(), "index");
        if (t->kind == TY_ARRAY) return e->type = t->elem;
        if (t->kind == TY_STRING) return e->type = ty_int();
        fatal_at(e->loc, "%s is not indexable", type_str(t));
    }
    case EX_MEMBER: return check_member(c, e);
    case EX_NEW: {
        if (e->cast_type) { /* new Map<string, T>() */
            resolve_type(e->cast_type, c->mod);
            Type *vt = e->cast_type->elem;
            if (vt->kind == TY_STRUCT)
                fatal_at(e->loc, "struct map values are not supported in v0.1");
            return e->type = e->cast_type;
        }
        Decl *d = mod_lookup(c->mod, e->name);
        if (!d || d->kind != D_CLASS) fatal_at(e->loc, "unknown class '%s'", e->name);
        ClassDecl *cd = d->cd;
        e->cls = cd;
        FnDecl *ctor = NULL;
        for (ClassDecl *k = cd; k && !ctor; k = k->parent) ctor = k->ctor;
        if (ctor) {
            check_args_against(c, e, ctor->params, ctor->nparams, e->name);
        } else if (e->nargs != 0) {
            fatal_at(e->loc, "class %s has no init taking arguments", e->name);
        }
        Type *t = mk_type(TY_CLASS);
        t->cdecl = cd;
        return e->type = t;
    }
    case EX_ARROW:
        check_arrow(c, e, expected);
        return e->type;
    case EX_ARRAYLIT: {
        Type *elem = NULL;
        if (expected && expected->kind == TY_ARRAY) elem = expected->elem;
        if (!elem && e->nargs == 0)
            fatal_at(e->loc, "cannot infer element type of empty array literal");
        for (int i = 0; i < e->nargs; i++) {
            check_expr(c, e->args[i], elem);
            if (!elem) elem = e->args[i]->type;
            else want(c, e->args[i], elem, "array element");
        }
        if (elem->kind == TY_NULL) fatal_at(e->loc, "cannot infer array element type from null");
        Type *t = mk_type(TY_ARRAY);
        t->elem = elem;
        return e->type = t;
    }
    case EX_AS: {
        Type *src = check_expr(c, e->lhs, NULL);
        resolve_type(e->cast_type, c->mod);
        Type *dst = e->cast_type;
        if (type_is_num(src) && type_is_num(dst)) return e->type = dst;
        if (src->kind == TY_CLASS && dst->kind == TY_CLASS) {
            if (!class_derives(src->cdecl, dst->cdecl) && !class_derives(dst->cdecl, src->cdecl))
                fatal_at(e->loc, "unrelated classes %s and %s", type_str(src), type_str(dst));
            return e->type = dst;
        }
        if ((src->kind == TY_CSTRING && dst->kind == TY_RAWPTR) ||
            (src->kind == TY_RAWPTR && dst->kind == TY_CSTRING))
            return e->type = dst;
        if (src->kind == TY_ARRAY && dst->kind == TY_RAWPTR)
            return e->type = dst; /* borrowed pointer to the array's element buffer */
        if (src->kind == TY_ARRAY && dst->kind == TY_CSTRING &&
            (norm_kind(src->elem->kind) == TY_BYTE || norm_kind(src->elem->kind) == TY_I8))
            return e->type = dst; /* byte[]/i8[] as a borrowed C string view */
        if (src->kind == TY_FN && dst->kind == TY_RAWPTR)
            return e->type = dst; /* closure as userdata for cthunk callbacks (borrowed) */
        fatal_at(e->loc, "cannot cast %s to %s", type_str(src), type_str(dst));
    }
    case EX_STRCONV:
        return e->type; /* checker-inserted, already typed */
    }
    fatal_at(e->loc, "internal: unchecked expression kind");
    return NULL;
}

/* ---------------- statements ---------------- */

static void check_stmt(Ctx *c, Stmt *s) {
    switch (s->kind) {
    case ST_LET: {
        if (s->decl_type) resolve_type(s->decl_type, c->mod);
        Type *t = check_expr(c, s->init, s->decl_type);
        if (s->decl_type) {
            want(c, s->init, s->decl_type, "let");
            t = s->decl_type;
        }
        if (t->kind == TY_VOID) fatal_at(s->loc, "cannot store void");
        if (t->kind == TY_NULL) fatal_at(s->loc, "cannot infer type from null; annotate the let");
        s->local = define_local(c, s->name, t, false, s->loc);
        return;
    }
    case ST_EXPR:
        check_expr(c, s->expr, NULL);
        return;
    case ST_IF:
        check_expr(c, s->expr, NULL);
        if (s->expr->type->kind != TY_BOOL) fatal_at(s->expr->loc, "if condition must be bool");
        scope_push(c);
        check_block(c, s->body, s->nbody);
        scope_pop(c);
        if (s->els) {
            scope_push(c);
            check_block(c, s->els, s->nels);
            scope_pop(c);
        }
        return;
    case ST_WHILE:
        check_expr(c, s->expr, NULL);
        if (s->expr->type->kind != TY_BOOL) fatal_at(s->expr->loc, "while condition must be bool");
        c->loop_depth++;
        scope_push(c);
        check_block(c, s->body, s->nbody);
        scope_pop(c);
        c->loop_depth--;
        return;
    case ST_FOR_RANGE:
        check_expr(c, s->range_lo, NULL);
        want(c, s->range_lo, ty_int(), "range");
        check_expr(c, s->range_hi, NULL);
        want(c, s->range_hi, ty_int(), "range");
        c->loop_depth++;
        scope_push(c);
        s->local = define_local(c, s->name, ty_int(), false, s->loc);
        check_block(c, s->body, s->nbody);
        scope_pop(c);
        c->loop_depth--;
        return;
    case ST_FOR_EACH: {
        Type *t = check_expr(c, s->iter, NULL);
        if (t->kind != TY_ARRAY) fatal_at(s->iter->loc, "for-in needs an array");
        c->loop_depth++;
        scope_push(c);
        s->local = define_local(c, s->name, t->elem, false, s->loc);
        check_block(c, s->body, s->nbody);
        scope_pop(c);
        c->loop_depth--;
        return;
    }
    case ST_RETURN:
        if (s->expr) {
            if (c->fn->ret->kind == TY_VOID) fatal_at(s->loc, "void function returns a value");
            check_expr(c, s->expr, c->fn->ret);
            want(c, s->expr, c->fn->ret, "return");
        } else if (c->fn->ret->kind != TY_VOID) {
            fatal_at(s->loc, "missing return value");
        }
        return;
    case ST_BREAK: case ST_CONTINUE:
        if (!c->loop_depth) fatal_at(s->loc, "break/continue outside a loop");
        return;
    case ST_BLOCK:
        scope_push(c);
        check_block(c, s->body, s->nbody);
        scope_pop(c);
        return;
    }
}

static void check_block(Ctx *c, Stmt **body, int n) {
    for (int i = 0; i < n; i++) check_stmt(c, body[i]);
}

/* ---------------- functions & modules ---------------- */

static void check_fn_body(Module *m, FnDecl *fd, ClassDecl *cls) {
    Ctx c = {0};
    c.mod = m;
    c.fn = fd;
    c.cls = cls;
    scope_push(&c);
    if (cls) {
        Type *t = mk_type(TY_CLASS);
        t->cdecl = cls;
        Local *self = define_local(&c, intern("this"), t, true, fd->loc);
        self->cname = "self";
        self->is_this = true;
    }
    for (int i = 0; i < fd->nparams; i++)
        fd->params[i].local =
            define_local(&c, fd->params[i].name, fd->params[i].type, true, fd->params[i].loc);
    check_block(&c, fd->body, fd->nbody);
    scope_pop(&c);
}

static void check_const(Module *m, Decl *d) {
    Expr *e = d->const_init;
    if (e->kind != EX_INT && e->kind != EX_FLOAT && e->kind != EX_BOOL && e->kind != EX_NULL &&
        !(e->kind == EX_UN && e->op == T_MINUS &&
          (e->lhs->kind == EX_INT || e->lhs->kind == EX_FLOAT)))
        fatal_at(e->loc, "const initializer must be a literal or null");
    Ctx c = {0};
    c.mod = m;
    FnDecl dummy = {0};
    c.fn = &dummy;
    Scope sc = {0};
    c.scope = &sc;
    check_expr(&c, e, d->const_type);
    if (!assignable(d->const_type, e->type, e))
        fatal_at(e->loc, "const initializer type mismatch");
}

void check_all(Module *mods, Module *entry) {
    /* duplicate top-level names within each module */
    for (Module *m = mods; m; m = m->next) {
        for (int i = 0; i < m->ndecls; i++) {
            Decl *a = m->decls[i];
            if (a->kind == D_IMPORT || a->kind == D_LINK) continue;
            for (int j = 0; j < i; j++) {
                Decl *b = m->decls[j];
                if (b->kind == D_IMPORT || b->kind == D_LINK) continue;
                if (a->name == b->name)
                    fatal_at(a->loc, "duplicate top-level name '%s'", a->name);
            }
        }
    }
    for (Module *m = mods; m; m = m->next) resolve_module_sigs(m);
    for (Module *m = mods; m; m = m->next)
        for (int i = 0; i < m->ndecls; i++)
            if (m->decls[i]->kind == D_CLASS) layout_class(m->decls[i]->cd);
    for (Module *m = mods; m; m = m->next) {
        for (int i = 0; i < m->ndecls; i++) {
            Decl *d = m->decls[i];
            switch (d->kind) {
            case D_CONST: check_const(m, d); break;
            case D_FN: check_fn_body(m, d->fn, NULL); break;
            case D_CLASS: {
                ClassDecl *cd = d->cd;
                for (int mi = 0; mi < cd->nmethods; mi++)
                    check_fn_body(m, cd->methods[mi], cd);
                if (cd->ctor) check_fn_body(m, cd->ctor, cd);
                if (cd->deinit_body) {
                    FnDecl *dd = NEW(FnDecl);
                    dd->name = intern("deinit");
                    dd->loc = cd->loc;
                    dd->module = m;
                    dd->owner = cd;
                    dd->ret = ty_void();
                    dd->vslot = -1;
                    dd->body = cd->deinit_body;
                    dd->nbody = cd->ndeinit;
                    check_fn_body(m, dd, cd);
                }
                break;
            }
            default: break;
            }
        }
    }
    Decl *maind = mod_lookup_own(entry, intern("main"));
    if (!maind || maind->kind != D_FN)
        fatal("entry module '%s' must define fn main()", entry->name);
    if (maind->fn->nparams != 0 || maind->fn->ret->kind != TY_VOID)
        fatal_at(maind->loc, "fn main must take no parameters and return nothing");
}
