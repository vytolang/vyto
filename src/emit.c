#include "emit.h"
#include "check.h"
#include "lex.h"

/* ---------------- names & types ---------------- */

/* Overflow-checked arithmetic on/off (on for debug, off for --release). */
static bool g_checks = false;

/* Signed integer types that get overflow checks, with their C range macros.
   Unsigned types wrap by definition; clong/culong are FFI target-width. */
static bool int_bounds(const Type *t, const char **lo, const char **hi) {
    switch (t->kind) {
    case TY_INT: case TY_I64: *lo = "INT64_MIN"; *hi = "INT64_MAX"; return true;
    case TY_I32: *lo = "INT32_MIN"; *hi = "INT32_MAX"; return true;
    case TY_I16: *lo = "INT16_MIN"; *hi = "INT16_MAX"; return true;
    case TY_I8:  *lo = "INT8_MIN";  *hi = "INT8_MAX";  return true;
    default: return false;
    }
}

static int type_bits(const Type *t) {
    switch (t->kind) {
    case TY_BYTE: case TY_U8: case TY_I8: return 8;
    case TY_I16: case TY_U16: return 16;
    case TY_I32: case TY_U32: return 32;
    default: return 64; /* int, i64, u64, clong, culong */
    }
}

static bool type_unsigned_int(const Type *t) {
    switch (t->kind) {
    case TY_BYTE: case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_CULONG:
        return true;
    default: return false;
    }
}

static const char *struct_cname(StructDecl *sd) {
    if (sd->is_extern) return sd->name;
    return arena_printf(&g_arena, "v_%s_%s", sd->module->name, sd->name);
}

static const char *class_cname(ClassDecl *cd) {
    return arena_printf(&g_arena, "v_%s_%s", cd->module->name, cd->name);
}

static const char *c_type(Type *t) {
    switch (t->kind) {
    case TY_VOID: return "void";
    case TY_INT: case TY_I64: return "int64_t";
    case TY_FLOAT: case TY_F64: return "double";
    case TY_BOOL: return "bool";
    case TY_BYTE: case TY_U8: return "uint8_t";
    case TY_I8: return "int8_t";
    case TY_I16: return "int16_t";
    case TY_I32: return "int32_t";
    case TY_U16: return "uint16_t";
    case TY_U32: return "uint32_t";
    case TY_U64: return "uint64_t";
    case TY_CLONG: return "long";
    case TY_CULONG: return "unsigned long";
    case TY_F32: return "float";
    case TY_STRING: return "VtString*";
    case TY_CSTRING: return "const char*";
    case TY_RAWPTR: case TY_NULL: return "void*";
    case TY_ARRAY: return "VtArray*";
    case TY_MAP: return "VtMap*";
    case TY_FN: return "VtClosure*";
    case TY_STRUCT: return struct_cname(t->sdecl);
    case TY_CLASS: return arena_printf(&g_arena, "%s*", class_cname(t->cdecl));
    case TY_NAMED: break;
    }
    fatal("internal: unresolved type in emitter");
    return NULL;
}

static const char *fn_cname(FnDecl *fd) {
    /* extern fns get a private C identifier aliased to the real symbol via
       __asm__, so system headers can never clash with our declaration */
    if (fd->is_extern) return arena_printf(&g_arena, "vx_%s", fd->name);
    if (fd->owner)
        return arena_printf(&g_arena, "v_%s_%s_%s", fd->module->name, fd->owner->name, fd->name);
    if (fd->sowner)
        return arena_printf(&g_arena, "v_%s_%s_%s", fd->module->name, fd->sowner->name, fd->name);
    return arena_printf(&g_arena, "v_%s_%s", fd->module->name, fd->name);
}

static const char *c_escape(const char *s, size_t n) {
    SBuf sb;
    sb_init(&sb);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') sb_printf(&sb, "\\%c", c);
        else if (c >= 32 && c < 127) sb_putc(&sb, (char)c);
        else sb_printf(&sb, "\\%03o", c);
    }
    char *out = arena_strdup(&g_arena, sb.data);
    sb_free(&sb);
    return out;
}

/* ---------------- emitter state ---------------- */

typedef struct EScope {
    const char **names;
    int n, cap;
    bool is_loop;
    struct EScope *parent;
} EScope;

typedef struct Em {
    Module *mod;
    SBuf *out;      /* function body */
    SBuf *pre;      /* declarations at top of function */
    SBuf *aux;      /* module-level helpers (arrows, wrappers), shared */
    FnDecl *fn;
    ClassDecl *cls;
    int tempc;
    int indent;
    const char **stmt_temps; /* owned temps awaiting release at stmt end */
    int nstmt, stmtcap;
    EScope *scope;
} Em;

static void ind(Em *em) {
    for (int i = 0; i < em->indent; i++) sb_puts(em->out, "    ");
}

static void epush(Em *em, bool is_loop) {
    EScope *s = NEW(EScope);
    s->parent = em->scope;
    s->is_loop = is_loop;
    em->scope = s;
}

static void ereg(Em *em, const char *cname, Type *t) {
    if (!type_is_ref(t)) return;
    EScope *s = em->scope;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        const char **nn = arena_alloc(&g_arena, sizeof(char *) * (size_t)nc);
        memcpy(nn, s->names, sizeof(char *) * (size_t)s->n);
        s->names = nn;
        s->cap = nc;
    }
    s->names[s->n++] = cname;
}

static void escope_release(Em *em, EScope *s) {
    for (int i = s->n - 1; i >= 0; i--) {
        ind(em);
        sb_printf(em->out, "VT_RELEASE(%s);\n", s->names[i]);
    }
}

static void epop(Em *em) {
    escope_release(em, em->scope);
    em->scope = em->scope->parent;
}

static const char *newtemp(Em *em, Type *t, bool owned) {
    const char *name = arena_printf(&g_arena, "_t%d", em->tempc++);
    if (type_is_ref(t) || t->kind == TY_CSTRING || t->kind == TY_RAWPTR)
        sb_printf(em->pre, "    %s %s = 0;\n", c_type(t), name);
    else
        sb_printf(em->pre, "    %s %s;\n", c_type(t), name);
    if (owned) {
        if (em->nstmt == em->stmtcap) {
            int nc = em->stmtcap ? em->stmtcap * 2 : 8;
            const char **nn = arena_alloc(&g_arena, sizeof(char *) * (size_t)nc);
            memcpy(nn, em->stmt_temps, sizeof(char *) * (size_t)em->nstmt);
            em->stmt_temps = nn;
            em->stmtcap = nc;
        }
        em->stmt_temps[em->nstmt++] = name;
    }
    return name;
}

static void flush_temps(Em *em, int wm, bool emit_code) {
    if (emit_code)
        for (int i = em->nstmt - 1; i >= wm; i--) {
            ind(em);
            sb_printf(em->out, "VT_RELEASE(%s);\n", em->stmt_temps[i]);
        }
    em->nstmt = wm;
}

/* Ref-typed params arrive borrowed from the caller. If the body assigns to
   one, the assignment releases the old value — so take a defensive +1 on
   entry and register it for release on every exit path. Without this,
   assigning to a ref param frees the caller's reference (use-after-free). */
static void retain_assigned_params(Em *em, FnDecl *fd) {
    for (int i = 0; i < fd->nparams; i++) {
        Local *l = fd->params[i].local;
        if (l && l->assigned && type_is_ref(l->type)) {
            ind(em);
            sb_printf(em->out, "vt_retain(%s);\n", l->cname);
            ereg(em, l->cname, l->type);
        }
    }
}

/* ---------------- expressions ---------------- */

static char *ex(Em *em, Expr *e, bool *fresh);
static void emit_arrow_defs(Em *em, Expr *e);
static void emit_stmt(Em *em, Stmt *s);
static void emit_stmts(Em *em, Stmt **body, int n);

/* borrowed value: fresh results are spilled to an owned temp released at stmt end */
static char *ex_b(Em *em, Expr *e) {
    bool fresh = false;
    char *f = ex(em, e, &fresh);
    if (fresh && type_is_ref(e->type)) {
        const char *t = newtemp(em, e->type, true);
        return arena_printf(&g_arena, "(%s = %s)", t, f);
    }
    return f;
}

static char *cast_to(char *frag, Type *from, Type *to) {
    if (!to || !from) return frag;
    if (from->kind == TY_CLASS && to->kind == TY_CLASS && from->cdecl != to->cdecl)
        return arena_printf(&g_arena, "((%s)(%s))", c_type(to), frag);
    return frag;
}

/* borrowed value cast to destination type (args, reads into typed slots) */
static char *ex_v(Em *em, Expr *e, Type *target) {
    return cast_to(ex_b(em, e), e->type, target);
}

/* owned (+1) value, NOT registered for release: caller consumes it */
static char *ex_o(Em *em, Expr *e, Type *target) {
    if (!type_is_ref(e->type)) return ex_v(em, e, target);
    bool fresh = false;
    char *f = ex(em, e, &fresh);
    if (!fresh)
        f = arena_printf(&g_arena, "((%s)vt_retain(%s))", c_type(e->type), f);
    return cast_to(f, e->type, target);
}

static char *args_list(Em *em, Expr **args, int n, Param *params, Type **ptypes) {
    SBuf sb;
    sb_init(&sb);
    for (int i = 0; i < n; i++) {
        if (i) sb_puts(&sb, ", ");
        Type *pt = params ? params[i].type : (ptypes ? ptypes[i] : NULL);
        sb_puts(&sb, ex_v(em, args[i], pt));
    }
    char *out = arena_strdup(&g_arena, sb.data);
    sb_free(&sb);
    return out;
}

static char *fnptr_sig(Type *ret, const char *self_ct, Type **params, int nparams,
                       Param *pparams) {
    SBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s (*)(%s", c_type(ret), self_ct);
    for (int i = 0; i < nparams; i++)
        sb_printf(&sb, ", %s", c_type(pparams ? pparams[i].type : params[i]));
    sb_puts(&sb, ")");
    char *out = arena_strdup(&g_arena, sb.data);
    sb_free(&sb);
    return out;
}

static char *bits_of(Em *em, Expr *e, Type *t) {
    char *v = ex_v(em, e, t);
    if (type_is_ref(t)) return arena_printf(&g_arena, "((uint64_t)(uintptr_t)%s)", v);
    if (type_is_float(t)) return arena_printf(&g_arena, "vt_f64bits(%s)", v);
    return arena_printf(&g_arena, "((uint64_t)(%s))", v);
}

static char *unbits(char *frag, Type *t) {
    if (type_is_ref(t))
        return arena_printf(&g_arena, "((%s)(uintptr_t)%s)", c_type(t), frag);
    if (type_is_float(t)) return arena_printf(&g_arena, "vt_bits2f64(%s)", frag);
    return arena_printf(&g_arena, "((%s)%s)", c_type(t), frag);
}

static char *strconv_frag(Em *em, Expr *inner) {
    Type *t = inner->type;
    if (t->kind == TY_STRING) return ex_b(em, inner); /* callers wrap only non-strings */
    char *v = ex_v(em, inner, NULL);
    if (t->kind == TY_CSTRING) return arena_printf(&g_arena, "vt_str_from_cstr(%s)", v);
    if (type_is_float(t)) return arena_printf(&g_arena, "vt_str_from_float(%s)", v);
    if (t->kind == TY_BOOL) return arena_printf(&g_arena, "vt_str_from_bool(%s)", v);
    return arena_printf(&g_arena, "vt_str_from_int((int64_t)(%s))", v);
}

static char *emit_call(Em *em, Expr *e, bool *fresh) {
    *fresh = type_is_ref(e->type);

    if (e->is_super_call) {
        FnDecl *pc = NULL;
        for (ClassDecl *k = e->cls; k && !pc; k = k->parent) pc = k->ctor;
        *fresh = false;
        if (!pc) return arena_strdup(&g_arena, "(void)0");
        return arena_printf(&g_arena, "%s((%s*)self%s%s)", fn_cname(pc), class_cname(pc->owner),
                            e->nargs ? ", " : "", args_list(em, e->args, e->nargs, pc->params, NULL));
    }

    if (e->ref == REF_BUILTIN) {
        Expr **a = e->args;
        Expr *recv = e->lhs && e->lhs->kind == EX_MEMBER ? e->lhs->lhs : NULL;
        switch ((BuiltinKind)e->builtin) {
        case B_PRINT:
            *fresh = false;
            return arena_printf(&g_arena, "vt_print(%s)", ex_b(em, a[0]));
        case B_PANIC:
            *fresh = false;
            return arena_printf(&g_arena, "vt_panic(\"%s\", %d, %s)",
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line,
                                ex_b(em, a[0]));
        case B_STR:
            *fresh = true;
            return strconv_frag(em, a[0]);
        case B_PUSH: {
            Type *et = recv->type->elem;
            const char *tv = newtemp(em, et, false); /* borrowed value; push retains internally */
            *fresh = false;
            return arena_printf(&g_arena, "(%s = %s, vt_arr_push_at(%s, &%s, \"%s\", %d))", tv,
                                ex_v(em, a[0], et), ex_b(em, recv), tv,
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        }
        case B_POP: {
            Type *et = recv->type->elem;
            const char *tv = newtemp(em, et, false); /* ownership transfers to consumer */
            *fresh = type_is_ref(et);
            return arena_printf(&g_arena, "(vt_arr_pop(%s, &%s, \"%s\", %d), %s)", ex_b(em, recv),
                                tv, c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line, tv);
        }
        case B_MAP_SET:
            *fresh = false;
            return arena_printf(&g_arena, "vt_map_set(%s, %s, %s, \"%s\", %d)", ex_b(em, recv),
                                ex_b(em, a[0]), bits_of(em, a[1], recv->type->elem),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        case B_MAP_GET:
            *fresh = false; /* borrowed */
            return unbits(arena_printf(&g_arena, "vt_map_get(%s, %s, \"%s\", %d)", ex_b(em, recv),
                                       ex_b(em, a[0]), c_escape(e->loc.file, strlen(e->loc.file)),
                                       e->loc.line),
                          recv->type->elem);
        case B_MAP_HAS:
            *fresh = false;
            return arena_printf(&g_arena, "vt_map_has(%s, %s, \"%s\", %d)", ex_b(em, recv),
                                ex_b(em, a[0]), c_escape(e->loc.file, strlen(e->loc.file)),
                                e->loc.line);
        case B_MAP_REMOVE:
            *fresh = false;
            return arena_printf(&g_arena, "vt_map_remove(%s, %s, \"%s\", %d)", ex_b(em, recv),
                                ex_b(em, a[0]), c_escape(e->loc.file, strlen(e->loc.file)),
                                e->loc.line);
        case B_CSTR:
            *fresh = false;
            return arena_printf(&g_arena, "vt_str_cstr(%s)", ex_b(em, recv));
        case B_CTHUNK:
        case B_CTHUNK_LAST: {
            bool ud_first = e->builtin == B_CTHUNK;
            Type *ft = a[0]->type;
            int id = ++em->mod->arrow_counter;
            SBuf *ax = em->aux;
            sb_printf(ax, "static %s __vt_cthunk%d(", c_type(ft->ret), id);
            if (ud_first) sb_puts(ax, "void* _ud");
            for (int i = 0; i < ft->nparams; i++)
                sb_printf(ax, "%s%s a%d", (i || ud_first) ? ", " : "", c_type(ft->params[i]), i);
            if (!ud_first) sb_printf(ax, "%svoid* _ud", ft->nparams ? ", " : "");
            if (ud_first && ft->nparams == 0) { /* nothing more */ }
            sb_puts(ax, ") {\n    VtClosure* _c = (VtClosure*)_ud;\n    ");
            if (ft->ret->kind != TY_VOID) sb_puts(ax, "return ");
            sb_printf(ax, "((%s)_c->fn)(_c->env", fnptr_sig(ft->ret, "VtObj*", ft->params,
                                                            ft->nparams, NULL));
            for (int i = 0; i < ft->nparams; i++) sb_printf(ax, ", a%d", i);
            sb_puts(ax, ");\n}\n");
            *fresh = false;
            /* evaluate the closure expression for effect/validity, discard the value */
            char *cv = ex_b(em, a[0]);
            return arena_printf(&g_arena, "((void)(%s), (void*)__vt_cthunk%d)", cv, id);
        }
        case B_SLICE:
            *fresh = true;
            return arena_printf(&g_arena, "vt_str_slice(%s, %s, %s, \"%s\", %d)", ex_b(em, recv),
                                ex_b(em, a[0]), ex_b(em, a[1]),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        case B_READFILE:
            *fresh = true;
            return arena_printf(&g_arena, "vt_file_read(%s, \"%s\", %d)", ex_b(em, a[0]),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        case B_READLINES:
            *fresh = true;
            return arena_printf(&g_arena, "vt_file_lines(%s, \"%s\", %d)", ex_b(em, a[0]),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        case B_LISTDIR:
            *fresh = true;
            return arena_printf(&g_arena, "vt_dir_list(%s, \"%s\", %d)", ex_b(em, a[0]),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        case B_ARGS:
            *fresh = true;
            return arena_printf(&g_arena, "vt_args()");
        case B_ISDIR:
            *fresh = false;
            return arena_printf(&g_arena, "vt_is_dir(%s)", ex_b(em, a[0]));
        case B_WRITEFILE:
        case B_APPENDFILE:
            *fresh = false;
            return arena_printf(&g_arena, "vt_file_write(%s, %s, %s)", ex_b(em, a[0]),
                                ex_b(em, a[1]), e->builtin == B_APPENDFILE ? "true" : "false");
        case B_BYTES:
            *fresh = true;
            return arena_printf(&g_arena, "vt_arr_bytes(%s)", ex_b(em, a[0]));
        default: break;
        }
        fatal_at(e->loc, "internal: bad builtin");
    }

    if (e->ref == REF_GLOBAL_FN || e->ref == REF_EXTERN_FN) {
        FnDecl *fd = e->decl->fn;
        return arena_printf(&g_arena, "%s(%s)", fn_cname(fd),
                            args_list(em, e->args, e->nargs, fd->params, NULL));
    }

    if (e->ref == REF_METHOD) {
        FnDecl *m = e->method;
        Expr *recv = e->lhs->lhs;
        if (m->sowner) {
            /* struct method: receiver passed by value, no vtable, no retain */
            char *rv = ex_v(em, recv, recv->type);
            char *args = args_list(em, e->args, e->nargs, m->params, NULL);
            return arena_printf(&g_arena, "%s(%s%s%s)", fn_cname(m), rv,
                                e->nargs ? ", " : "", args);
        }
        const char *owner_ct = arena_printf(&g_arena, "%s*", class_cname(m->owner));
        if (m->vslot >= 0) {
            const char *tr = newtemp(em, recv->type, false);
            char *rv = ex_b(em, recv);
            char *sig = fnptr_sig(m->ret, owner_ct, NULL, m->nparams, m->params);
            char *args = args_list(em, e->args, e->nargs, m->params, NULL);
            return arena_printf(&g_arena,
                                "(%s = %s, ((%s)((VtObj*)%s)->type->vtbl[%d])((%s)%s%s%s))", tr, rv,
                                sig, tr, m->vslot, owner_ct, tr, e->nargs ? ", " : "", args);
        }
        char *rv = ex_b(em, recv);
        char *call = arena_printf(&g_arena, "%s((%s)%s%s%s)", fn_cname(m), owner_ct, rv,
                                  e->nargs ? ", " : "", args_list(em, e->args, e->nargs, m->params, NULL));
        /* a builder returns the declaring class's pointer; re-cast to the
           receiver's static type so a chained call keeps its concrete type */
        if (m->is_builder)
            return arena_printf(&g_arena, "((%s)%s)", c_type(e->type), call);
        return call;
    }

    if (e->sd) { /* struct constructor */
        if (e->nargs == 0)
            return arena_printf(&g_arena, "((%s){0})", struct_cname(e->sd));
        SBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "((%s){", struct_cname(e->sd));
        for (int i = 0; i < e->nargs; i++) {
            if (i) sb_puts(&sb, ", ");
            const char *fname = e->sd->is_extern ? e->sd->fields[i].name
                                                 : arena_printf(&g_arena, "f_%s", e->sd->fields[i].name);
            sb_printf(&sb, ".%s = %s", fname, ex_v(em, e->args[i], e->sd->fields[i].type));
        }
        sb_puts(&sb, "})");
        char *out = arena_strdup(&g_arena, sb.data);
        sb_free(&sb);
        *fresh = false;
        return out;
    }

    /* closure call */
    Type *ft = e->lhs->type;
    const char *tc = newtemp(em, ft, false);
    char *cv = ex_b(em, e->lhs);
    char *sig = fnptr_sig(ft->ret, "VtObj*", ft->params, ft->nparams, NULL);
    char *args = args_list(em, e->args, e->nargs, NULL, ft->params);
    return arena_printf(&g_arena, "(%s = %s, ((%s)((VtClosure*)%s)->fn)(((VtClosure*)%s)->env%s%s))",
                        tc, cv, sig, tc, tc, e->nargs ? ", " : "", args);
}

static char *ex(Em *em, Expr *e, bool *fresh) {
    *fresh = false;
    switch (e->kind) {
    case EX_INT:
        if (e->type && type_is_float(e->type))
            return arena_printf(&g_arena, "%lld.0", (long long)e->ival);
        return arena_printf(&g_arena, "%lldLL", (long long)e->ival);
    case EX_FLOAT: {
        char buf[64];
        snprintf(buf, sizeof buf, "%.17g", e->fval);
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n'))
            strcat(buf, ".0");
        return arena_strdup(&g_arena, buf);
    }
    case EX_BOOL: return arena_strdup(&g_arena, e->ival ? "true" : "false");
    case EX_NULL: return arena_strdup(&g_arena, "0");
    case EX_STR: {
        const char *v = arena_printf(&g_arena, "_sl%d", em->tempc++);
        sb_printf(em->pre, "    static VtString* %s;\n", v);
        return arena_printf(&g_arena, "(%s ? %s : (%s = vt_str_immortal(\"%s\", %d)))", v, v, v,
                            c_escape(e->sval, e->slen), (int)e->slen);
    }
    case EX_THIS: return arena_strdup(&g_arena, "self");
    case EX_IDENT:
        switch (e->ref) {
        case REF_LOCAL: case REF_PARAM: return arena_strdup(&g_arena, e->local->cname);
        case REF_CAPTURE: return arena_printf(&g_arena, "_env->c_%s", e->name);
        case REF_CONST:
            return arena_printf(&g_arena, "v_%s_%s", e->decl->module->name, e->name);
        case REF_GLOBAL_FN: {
            /* function used as a closure value: emit a thunk once, and hand
               out a lazily-built singleton so `f == f` holds (stable identity) */
            FnDecl *fd = e->decl->fn;
            if (!e->decl->wrapper_emitted) {
                e->decl->wrapper_emitted = true;
                SBuf *ax = em->aux;
                sb_printf(ax, "static %s v_%s__w_%s(VtObj* _e", c_type(fd->ret), fd->module->name,
                          fd->name);
                for (int i = 0; i < fd->nparams; i++)
                    sb_printf(ax, ", %s a%d", c_type(fd->params[i].type), i);
                sb_puts(ax, ") { (void)_e; ");
                if (fd->ret->kind != TY_VOID) sb_puts(ax, "return ");
                sb_printf(ax, "%s(", fn_cname(fd));
                for (int i = 0; i < fd->nparams; i++) sb_printf(ax, "%sa%d", i ? ", " : "", i);
                sb_puts(ax, "); }\n");
                sb_printf(ax, "static VtClosure* v_%s__c_%s;\n", fd->module->name, fd->name);
            }
            *fresh = false; /* the cache owns its reference; consumers retain */
            const char *cv = arena_printf(&g_arena, "v_%s__c_%s", fd->module->name, fd->name);
            return arena_printf(&g_arena, "(%s ? %s : (%s = vt_closure_new((void*)v_%s__w_%s, 0)))",
                                cv, cv, cv, fd->module->name, fd->name);
        }
        default:
            fatal_at(e->loc, "internal: bad ident ref");
        }
        break;
    case EX_MEMBER: {
        if (e->ref == REF_BUILTIN && e->builtin == B_LEN)
            return arena_printf(&g_arena, "vt_len(%s, \"%s\", %d)", ex_b(em, e->lhs),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        if (e->sd) { /* struct field */
            const char *fname = e->sd->is_extern ? e->name
                                                 : arena_printf(&g_arena, "f_%s", e->name);
            return arena_printf(&g_arena, "(%s).%s", ex_b(em, e->lhs), fname);
        }
        /* class field */
        return arena_printf(&g_arena, "(((%s*)%s)->f_%s)", class_cname(e->cls), ex_b(em, e->lhs),
                            e->name);
    }
    case EX_INDEX: {
        if (e->lhs->type->kind == TY_STRING)
            return arena_printf(&g_arena, "vt_str_index(%s, %s, \"%s\", %d)", ex_b(em, e->lhs),
                                ex_b(em, e->rhs), c_escape(e->loc.file, strlen(e->loc.file)),
                                e->loc.line);
        Type *et = e->lhs->type->elem;
        return arena_printf(&g_arena, "(*(%s*)vt_arr_at(%s, %s, \"%s\", %d))", c_type(et),
                            ex_b(em, e->lhs), ex_b(em, e->rhs),
                            c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
    }
    case EX_UN: {
        if (e->op == T_AMP)
            return arena_printf(&g_arena, "((void*)&%s)", e->lhs->local->cname);
        const char *lo, *hi;
        if (g_checks && e->op == T_MINUS && int_bounds(e->type, &lo, &hi))
            return arena_printf(&g_arena, "vt_ck_neg(%s, %s, %s, \"%s\", %d)",
                                ex_b(em, e->lhs), lo, hi,
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        return arena_printf(&g_arena, "(%s%s)",
                            e->op == T_NOT ? "!" : e->op == T_TILDE ? "~" : "-",
                            ex_b(em, e->lhs));
    }
    case EX_BIN: {
        Type *lt = e->lhs->type, *rt = e->rhs->type;
        if (e->op == T_PLUS && e->type->kind == TY_STRING) {
            *fresh = true;
            return arena_printf(&g_arena, "vt_str_concat(%s, %s)", ex_b(em, e->lhs),
                                ex_b(em, e->rhs));
        }
        if ((e->op == T_EQ || e->op == T_NEQ) &&
            (lt->kind == TY_STRING && rt->kind == TY_STRING))
            return arena_printf(&g_arena, "(%svt_str_eq(%s, %s))", e->op == T_NEQ ? "!" : "",
                                ex_b(em, e->lhs), ex_b(em, e->rhs));
        if ((e->op == T_EQ || e->op == T_NEQ) && (type_is_ref(lt) || type_is_ref(rt)))
            return arena_printf(&g_arena, "((void*)(%s) %s (void*)(%s))", ex_b(em, e->lhs),
                                e->op == T_EQ ? "==" : "!=", ex_b(em, e->rhs));
        const char *op = NULL;
        switch (e->op) {
        case T_PLUS: op = "+"; break; case T_MINUS: op = "-"; break;
        case T_STAR: op = "*"; break; case T_SLASH: op = "/"; break;
        case T_PERCENT: op = "%"; break;
        case T_LT: op = "<"; break; case T_LE: op = "<="; break;
        case T_GT: op = ">"; break; case T_GE: op = ">="; break;
        case T_EQ: op = "=="; break; case T_NEQ: op = "!="; break;
        case T_ANDAND: op = "&&"; break; case T_OROR: op = "||"; break;
        case T_AMP: op = "&"; break; case T_PIPE: op = "|"; break;
        case T_CARET: op = "^"; break;
        case T_SHL: op = "<<"; break; case T_SHR: op = ">>"; break;
        }
        const char *lo, *hi;
        if (g_checks && (e->op == T_PLUS || e->op == T_MINUS || e->op == T_STAR) &&
            int_bounds(e->type, &lo, &hi)) {
            const char *fn = e->op == T_PLUS ? "vt_ck_add"
                           : e->op == T_MINUS ? "vt_ck_sub" : "vt_ck_mul";
            return arena_printf(&g_arena, "%s(%s, %s, %s, %s, \"%s\", %d)", fn,
                                ex_b(em, e->lhs), ex_b(em, e->rhs), lo, hi,
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        }
        /* shifts: guard the amount (negative / >= width is C UB) */
        if (g_checks && (e->op == T_SHL || e->op == T_SHR) && type_is_int(e->type)) {
            const char *fn = e->op == T_SHL ? "vt_ck_shl"
                           : type_unsigned_int(e->type) ? "vt_ck_shru" : "vt_ck_shr";
            return arena_printf(&g_arena, "((%s)%s(%s, %s, %d, \"%s\", %d))", c_type(e->type), fn,
                                ex_b(em, e->lhs), ex_b(em, e->rhs), type_bits(e->type),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        }
        return arena_printf(&g_arena, "(%s %s %s)", ex_b(em, e->lhs), op, ex_b(em, e->rhs));
    }
    case EX_CALL: return emit_call(em, e, fresh);
    case EX_NEW: {
        *fresh = true;
        if (e->cast_type) /* map */
            return arena_printf(&g_arena, "vt_map_new(%s)",
                                type_is_ref(e->type->elem) ? "true" : "false");
        ClassDecl *cd = e->cls;
        FnDecl *ctor = NULL;
        for (ClassDecl *k = cd; k && !ctor; k = k->parent) ctor = k->ctor;
        return arena_printf(&g_arena, "%s__new(%s)", class_cname(cd),
                            ctor ? args_list(em, e->args, e->nargs, ctor->params, NULL) : "");
    }
    case EX_ARRAYLIT: {
        *fresh = true;
        Type *et = e->type->elem;
        const char *ta = newtemp(em, e->type, false);
        SBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "(%s = vt_arr_new(sizeof(%s), %s)", ta, c_type(et),
                  type_is_ref(et) ? "true" : "false");
        for (int i = 0; i < e->nargs; i++) {
            const char *tv = newtemp(em, et, false);
            sb_printf(&sb, ", (%s = %s, vt_arr_push(%s, &%s))", tv, ex_v(em, e->args[i], et), ta, tv);
        }
        sb_printf(&sb, ", %s)", ta);
        char *out = arena_strdup(&g_arena, sb.data);
        sb_free(&sb);
        return out;
    }
    case EX_AS: {
        Type *src = e->lhs->type, *dst = e->type;
        if (src->kind == TY_CLASS && dst->kind == TY_CLASS) {
            if (class_derives(src->cdecl, dst->cdecl)) /* upcast */
                return arena_printf(&g_arena, "((%s)(%s))", c_type(dst), ex_b(em, e->lhs));
            return arena_printf(&g_arena, "((%s)vt_checked_cast(%s, &%s__type, \"%s\", %d))",
                                c_type(dst), ex_b(em, e->lhs), class_cname(dst->cdecl),
                                c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
        }
        if (src->kind == TY_ARRAY) /* byte[] as cstring/rawptr: borrowed buffer view */
            return arena_printf(&g_arena, "((%s)vt_arr_data(%s))", c_type(dst), ex_b(em, e->lhs));
        return arena_printf(&g_arena, "((%s)(%s))", c_type(dst), ex_b(em, e->lhs));
    }
    case EX_STRCONV:
        if (e->lhs->type->kind == TY_STRING) {
            bool f;
            char *r = ex(em, e->lhs, &f);
            *fresh = f;
            return r;
        }
        *fresh = true;
        return strconv_frag(em, e->lhs);
    case EX_ARROW: {
        emit_arrow_defs(em, e);
        FnDecl *fd = e->fn_lit;
        *fresh = true;
        if (fd->ncaptures == 0)
            return arena_printf(&g_arena, "vt_closure_new((void*)v_%s__arrow%d, 0)",
                                em->mod->name, fd->arrow_id);
        SBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "v_%s__mk%d(", em->mod->name, fd->arrow_id);
        for (int i = 0; i < fd->ncaptures; i++)
            sb_printf(&sb, "%s%s", i ? ", " : "", fd->captures[i].src->cname);
        sb_puts(&sb, ")");
        char *out = arena_strdup(&g_arena, sb.data);
        sb_free(&sb);
        return out;
    }
    case EX_ASSIGN:
        fatal_at(e->loc, "internal: assignment must be a statement");
    }
    fatal_at(e->loc, "internal: unhandled expr kind %d", e->kind);
    return NULL;
}

/* ---------------- arrows ---------------- */

static void emit_arrow_defs(Em *em, Expr *e) {
    FnDecl *fd = e->fn_lit;
    const char *mn = em->mod->name;
    int id = fd->arrow_id;
    SBuf *ax = em->aux;

    if (fd->ncaptures > 0) {
        sb_printf(ax, "typedef struct { VtObj hdr;");
        for (int i = 0; i < fd->ncaptures; i++)
            sb_printf(ax, " %s c_%s;", c_type(fd->captures[i].type), fd->captures[i].name);
        sb_printf(ax, " } v_%s__env%d;\n", mn, id);
        sb_printf(ax, "static void v_%s__env%d_deinit(void* p) {\n", mn, id);
        sb_printf(ax, "    v_%s__env%d* e = p; (void)e;\n", mn, id);
        for (int i = 0; i < fd->ncaptures; i++)
            if (type_is_ref(fd->captures[i].type))
                sb_printf(ax, "    vt_release(e->c_%s);\n", fd->captures[i].name);
        sb_puts(ax, "}\n");
        sb_printf(ax, "static const VtType v_%s__env%d_type = {\"env\", v_%s__env%d_deinit, 0, 0};\n",
                  mn, id, mn, id);
    }

    /* arrow function */
    SBuf proto;
    sb_init(&proto);
    sb_printf(&proto, "static %s v_%s__arrow%d(VtObj* _envv", c_type(fd->ret), mn, id);
    for (int i = 0; i < fd->nparams; i++)
        sb_printf(&proto, ", %s %s", c_type(fd->params[i].type), fd->params[i].local->cname);
    sb_puts(&proto, ")");

    SBuf body;
    sb_init(&body);
    Em aem = {0};
    aem.mod = em->mod;
    aem.aux = em->aux;
    aem.fn = fd;
    aem.cls = NULL;
    SBuf pre, out;
    sb_init(&pre);
    sb_init(&out);
    aem.pre = &pre;
    aem.out = &out;
    aem.indent = 1;
    epush(&aem, false);
    retain_assigned_params(&aem, fd);
    emit_stmts(&aem, fd->body, fd->nbody);
    escope_release(&aem, aem.scope);

    sb_printf(ax, "%s {\n", proto.data);
    if (fd->ncaptures > 0)
        sb_printf(ax, "    v_%s__env%d* _env = (v_%s__env%d*)_envv; (void)_env;\n", mn, id, mn, id);
    else
        sb_puts(ax, "    (void)_envv;\n");
    sb_puts(ax, pre.data);
    sb_puts(ax, out.data);
    if (fd->ret->kind != TY_VOID)
        sb_printf(ax, "    { %s _dead = {0}; return _dead; }\n", c_type(fd->ret));
    sb_puts(ax, "}\n");
    sb_free(&pre);
    sb_free(&out);
    sb_free(&proto);
    sb_free(&body);

    /* maker */
    if (fd->ncaptures > 0) {
        sb_printf(ax, "static VtClosure* v_%s__mk%d(", mn, id);
        for (int i = 0; i < fd->ncaptures; i++)
            sb_printf(ax, "%s%s a%d", i ? ", " : "", c_type(fd->captures[i].type), i);
        sb_puts(ax, ") {\n");
        sb_printf(ax, "    v_%s__env%d* e = vt_alloc(sizeof *e, &v_%s__env%d_type);\n", mn, id, mn, id);
        for (int i = 0; i < fd->ncaptures; i++) {
            if (type_is_ref(fd->captures[i].type))
                sb_printf(ax, "    e->c_%s = (%s)vt_retain(a%d);\n", fd->captures[i].name,
                          c_type(fd->captures[i].type), i);
            else
                sb_printf(ax, "    e->c_%s = a%d;\n", fd->captures[i].name, i);
        }
        sb_printf(ax, "    return vt_closure_new((void*)v_%s__arrow%d, (VtObj*)e);\n}\n", mn, id);
    }
}

/* ---------------- statements ---------------- */

static void emit_stmts(Em *em, Stmt **body, int n) {
    for (int i = 0; i < n; i++) emit_stmt(em, body[i]);
}

static void emit_assign(Em *em, Expr *e) {
    Expr *lhs = e->lhs;
    Type *lt = lhs->type;
    const char *cop = e->op == T_PLUSEQ ? "+=" : e->op == T_MINUSEQ ? "-="
                     : e->op == T_STAREQ ? "*=" : e->op == T_SLASHEQ ? "/="
                     : e->op == T_PERCENTEQ ? "%=" : e->op == T_AMPEQ ? "&="
                     : e->op == T_PIPEEQ ? "|=" : e->op == T_CARETEQ ? "^="
                     : e->op == T_SHLEQ ? "<<=" : e->op == T_SHREQ ? ">>=" : "=";

    /* string += x  →  s = concat(s, x) */
    bool str_append = e->op == T_PLUSEQ && lt->kind == TY_STRING;

    if (lhs->kind == EX_IDENT) {
        const char *n = lhs->local->cname;
        if (!type_is_ref(lt)) {
            const char *lo, *hi;
            if (g_checks && (e->op == T_PLUSEQ || e->op == T_MINUSEQ || e->op == T_STAREQ) &&
                int_bounds(lt, &lo, &hi)) {
                const char *fn = e->op == T_PLUSEQ ? "vt_ck_add"
                               : e->op == T_MINUSEQ ? "vt_ck_sub" : "vt_ck_mul";
                ind(em);
                sb_printf(em->out, "%s = %s(%s, %s, %s, %s, \"%s\", %d);\n", n, fn, n,
                          ex_v(em, e->rhs, lt), lo, hi,
                          c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
                return;
            }
            ind(em);
            sb_printf(em->out, "%s %s %s;\n", n, cop, ex_v(em, e->rhs, lt));
            return;
        }
        const char *t = newtemp(em, lt, false);
        ind(em);
        if (str_append)
            sb_printf(em->out, "%s = vt_str_concat(%s, %s);\n", t, n, ex_b(em, e->rhs));
        else
            sb_printf(em->out, "%s = %s;\n", t, ex_o(em, e->rhs, lt));
        ind(em);
        sb_printf(em->out, "vt_release(%s);\n", n);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", n, t);
        return;
    }

    bool ck_compound = g_checks &&
                       (e->op == T_PLUSEQ || e->op == T_MINUSEQ || e->op == T_STAREQ);
    const char *ck_fn = e->op == T_PLUSEQ ? "vt_ck_add"
                      : e->op == T_MINUSEQ ? "vt_ck_sub" : "vt_ck_mul";

    if (lhs->kind == EX_MEMBER) {
        if (lhs->sd) { /* struct field: plain C lvalue */
            const char *fname = lhs->sd->is_extern ? lhs->name
                                                   : arena_printf(&g_arena, "f_%s", lhs->name);
            const char *lo, *hi;
            if (ck_compound && int_bounds(lt, &lo, &hi)) {
                /* pointer temp: evaluate the receiver once */
                ind(em);
                sb_printf(em->out,
                          "{ %s *_pf = &(%s).%s; *_pf = %s(*_pf, %s, %s, %s, \"%s\", %d); }\n",
                          c_type(lt), ex_b(em, lhs->lhs), fname, ck_fn, ex_v(em, e->rhs, lt), lo,
                          hi, c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
                return;
            }
            ind(em);
            sb_printf(em->out, "(%s).%s %s %s;\n", ex_b(em, lhs->lhs), fname, cop,
                      ex_v(em, e->rhs, lt));
            return;
        }
        /* class field */
        Field *f = NULL;
        class_find_field(lhs->cls, lhs->name, &f);
        bool weak = f && f->type->weak;
        const char *tr = newtemp(em, lhs->lhs->type, false);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", tr, ex_b(em, lhs->lhs));
        const char *slot = arena_printf(&g_arena, "((%s*)%s)->f_%s", class_cname(lhs->cls), tr,
                                        lhs->name);
        if (!type_is_ref(lt)) {
            const char *lo, *hi;
            if (ck_compound && int_bounds(lt, &lo, &hi)) {
                ind(em);
                sb_printf(em->out, "%s = %s(%s, %s, %s, %s, \"%s\", %d);\n", slot, ck_fn, slot,
                          ex_v(em, e->rhs, lt), lo, hi,
                          c_escape(e->loc.file, strlen(e->loc.file)), e->loc.line);
                return;
            }
            ind(em);
            sb_printf(em->out, "%s %s %s;\n", slot, cop, ex_v(em, e->rhs, lt));
            return;
        }
        if (weak) {
            ind(em);
            sb_printf(em->out, "%s = %s;\n", slot, ex_v(em, e->rhs, f->type));
            return;
        }
        const char *tv = newtemp(em, lt, false);
        ind(em);
        if (str_append)
            sb_printf(em->out, "%s = vt_str_concat(%s, %s);\n", tv, slot, ex_b(em, e->rhs));
        else
            sb_printf(em->out, "%s = %s;\n", tv, ex_o(em, e->rhs, f ? f->type : lt));
        ind(em);
        sb_printf(em->out, "vt_release(%s);\n", slot);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", slot, tv);
        return;
    }

    /* index assignment: vt_arr_set handles retain/release */
    Expr *arr = lhs->lhs, *idx = lhs->rhs;
    Type *et = lt;
    const char *ta = newtemp(em, arr->type, false);
    const char *ti = newtemp(em, idx->type, false);
    const char *tv = newtemp(em, et, false);
    ind(em);
    sb_printf(em->out, "%s = %s;\n", ta, ex_b(em, arr));
    ind(em);
    sb_printf(em->out, "%s = %s;\n", ti, ex_b(em, idx));
    const char *fl = arena_printf(&g_arena, "\"%s\", %d",
                                  c_escape(lhs->loc.file, strlen(lhs->loc.file)), lhs->loc.line);
    if (e->op == T_ASSIGN) {
        ind(em);
        sb_printf(em->out, "%s = %s;\n", tv, ex_v(em, e->rhs, et));
    } else if (str_append) {
        ind(em);
        sb_printf(em->out, "%s = vt_str_concat(*(VtString**)vt_arr_at(%s, %s, %s), %s);\n", tv, ta,
                  ti, fl, ex_b(em, e->rhs));
    } else {
        const char *lo, *hi;
        if (ck_compound && int_bounds(et, &lo, &hi)) {
            ind(em);
            sb_printf(em->out, "%s = %s(*(%s*)vt_arr_at(%s, %s, %s), %s, %s, %s, %s);\n", tv,
                      ck_fn, c_type(et), ta, ti, fl, ex_v(em, e->rhs, et), lo, hi, fl);
        } else {
            /* the compound operator minus its trailing '=' is the plain binary op */
            char *bop = arena_strndup(&g_arena, cop, strlen(cop) - 1);
            ind(em);
            sb_printf(em->out, "%s = *(%s*)vt_arr_at(%s, %s, %s) %s %s;\n", tv, c_type(et), ta, ti,
                      fl, bop, ex_v(em, e->rhs, et));
        }
    }
    ind(em);
    sb_printf(em->out, "vt_arr_set(%s, %s, &%s, %s);\n", ta, ti, tv, fl);
    if (str_append) {
        ind(em);
        sb_printf(em->out, "VT_RELEASE(%s);\n", tv); /* concat result was +1; set retained it */
    }
}

static void release_for_jump(Em *em, bool through_loop) {
    for (EScope *s = em->scope; s; s = s->parent) {
        escope_release(em, s);
        if (through_loop && s->is_loop) break;
        if (!s->parent) break;
    }
}

static void emit_stmt(Em *em, Stmt *s) {
    int wm = em->nstmt;
    switch (s->kind) {
    case ST_LET: {
        Type *t = s->local->type;
        if (type_is_ref(t)) {
            ind(em);
            sb_printf(em->out, "%s %s = %s;\n", c_type(t), s->local->cname, ex_o(em, s->init, t));
            ereg(em, s->local->cname, t);
        } else {
            ind(em);
            sb_printf(em->out, "%s %s = %s;\n", c_type(t), s->local->cname, ex_v(em, s->init, t));
        }
        break;
    }
    case ST_EXPR:
        if (s->expr->kind == EX_ASSIGN) {
            emit_assign(em, s->expr);
        } else {
            char *f = ex_b(em, s->expr);
            ind(em);
            if (s->expr->type->kind == TY_VOID) sb_printf(em->out, "%s;\n", f);
            else sb_printf(em->out, "(void)(%s);\n", f);
        }
        break;
    case ST_IF: {
        const char *tc = newtemp(em, s->expr->type, false);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", tc, ex_b(em, s->expr));
        flush_temps(em, wm, true);
        ind(em);
        sb_printf(em->out, "if (%s) {\n", tc);
        em->indent++;
        epush(em, false);
        emit_stmts(em, s->body, s->nbody);
        epop(em);
        em->indent--;
        if (s->els) {
            ind(em);
            sb_puts(em->out, "} else {\n");
            em->indent++;
            epush(em, false);
            emit_stmts(em, s->els, s->nels);
            epop(em);
            em->indent--;
        }
        ind(em);
        sb_puts(em->out, "}\n");
        break;
    }
    case ST_WHILE: {
        ind(em);
        sb_puts(em->out, "for (;;) {\n");
        em->indent++;
        epush(em, true);
        int cwm = em->nstmt;
        const char *tc = newtemp(em, s->expr->type, false);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", tc, ex_b(em, s->expr));
        flush_temps(em, cwm, true);
        ind(em);
        sb_printf(em->out, "if (!%s) break;\n", tc);
        epush(em, false);
        emit_stmts(em, s->body, s->nbody);
        epop(em);
        epop(em);
        em->indent--;
        ind(em);
        sb_puts(em->out, "}\n");
        break;
    }
    case ST_FOR_RANGE: {
        const char *lo = newtemp(em, s->range_lo->type, false);
        const char *hi = newtemp(em, s->range_hi->type, false);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", lo, ex_b(em, s->range_lo));
        ind(em);
        sb_printf(em->out, "%s = %s;\n", hi, ex_b(em, s->range_hi));
        flush_temps(em, wm, true);
        ind(em);
        sb_printf(em->out, "for (int64_t %s = %s; %s < %s; %s++) {\n", s->local->cname, lo,
                  s->local->cname, hi, s->local->cname);
        em->indent++;
        epush(em, true);
        emit_stmts(em, s->body, s->nbody);
        epop(em);
        em->indent--;
        ind(em);
        sb_puts(em->out, "}\n");
        break;
    }
    case ST_FOR_EACH: {
        Type *et = s->local->type;
        const char *ta = newtemp(em, s->iter->type, false);
        ind(em);
        sb_printf(em->out, "%s = %s;\n", ta, ex_b(em, s->iter));
        ind(em);
        const char *iv = arena_printf(&g_arena, "_i%d", em->tempc++);
        sb_printf(em->out, "for (int64_t %s = 0; %s < vt_len(%s, \"%s\", %d); %s++) {\n", iv, iv,
                  ta, c_escape(s->loc.file, strlen(s->loc.file)), s->loc.line, iv);
        em->indent++;
        epush(em, true);
        ind(em);
        sb_printf(em->out, "%s %s = *(%s*)vt_arr_at(%s, %s, \"%s\", %d);\n", c_type(et),
                  s->local->cname, c_type(et), ta, iv, c_escape(s->loc.file, strlen(s->loc.file)),
                  s->loc.line);
        if (type_is_ref(et)) {
            ind(em);
            sb_printf(em->out, "vt_retain(%s);\n", s->local->cname);
            ereg(em, s->local->cname, et);
        }
        emit_stmts(em, s->body, s->nbody);
        epop(em);
        em->indent--;
        ind(em);
        sb_puts(em->out, "}\n");
        break;
    }
    case ST_RETURN: {
        if (s->expr && s->expr->type->kind != TY_VOID) {
            Type *rt = em->fn->ret;
            if (type_is_ref(rt)) {
                const char *tr = newtemp(em, rt, false);
                ind(em);
                sb_printf(em->out, "%s = %s;\n", tr, ex_o(em, s->expr, rt));
                flush_temps(em, wm, true);
                release_for_jump(em, false);
                ind(em);
                sb_printf(em->out, "return %s;\n", tr);
            } else {
                const char *tr = newtemp(em, rt, false);
                ind(em);
                sb_printf(em->out, "%s = %s;\n", tr, ex_v(em, s->expr, rt));
                flush_temps(em, wm, true);
                release_for_jump(em, false);
                ind(em);
                sb_printf(em->out, "return %s;\n", tr);
            }
        } else {
            flush_temps(em, wm, true);
            release_for_jump(em, false);
            ind(em);
            sb_puts(em->out, "return;\n");
        }
        em->nstmt = wm;
        return;
    }
    case ST_BREAK:
        release_for_jump(em, true);
        ind(em);
        sb_puts(em->out, "break;\n");
        return;
    case ST_CONTINUE:
        release_for_jump(em, true);
        ind(em);
        sb_puts(em->out, "continue;\n");
        return;
    case ST_BLOCK:
        ind(em);
        sb_puts(em->out, "{\n");
        em->indent++;
        epush(em, false);
        emit_stmts(em, s->body, s->nbody);
        epop(em);
        em->indent--;
        ind(em);
        sb_puts(em->out, "}\n");
        break;
    }
    flush_temps(em, wm, true);
}

/* ---------------- functions ---------------- */

static char *fn_proto(FnDecl *fd, bool with_names) {
    SBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s %s(", c_type(fd->ret), fn_cname(fd));
    bool first = true;
    if (fd->owner) {
        sb_printf(&sb, "%s* self", class_cname(fd->owner));
        first = false;
    } else if (fd->sowner) {
        sb_printf(&sb, "%s self", struct_cname(fd->sowner));
        first = false;
    }
    for (int i = 0; i < fd->nparams; i++) {
        if (!first) sb_puts(&sb, ", ");
        first = false;
        sb_printf(&sb, "%s", c_type(fd->params[i].type));
        if (with_names)
            sb_printf(&sb, " %s",
                      fd->params[i].local ? fd->params[i].local->cname
                                          : arena_printf(&g_arena, "p_%s", fd->params[i].name));
    }
    if (first) sb_puts(&sb, "void");
    sb_puts(&sb, ")");
    char *out = arena_strdup(&g_arena, sb.data);
    sb_free(&sb);
    return out;
}

static void emit_fn(Em *base, FnDecl *fd, ClassDecl *cls, SBuf *dst) {
    Em em = {0};
    em.mod = base->mod;
    em.aux = base->aux;
    em.fn = fd;
    em.cls = cls;
    SBuf pre, out;
    sb_init(&pre);
    sb_init(&out);
    em.pre = &pre;
    em.out = &out;
    em.indent = 1;
    epush(&em, false);
    retain_assigned_params(&em, fd);
    emit_stmts(&em, fd->body, fd->nbody);
    escope_release(&em, em.scope);

    sb_printf(dst, "%s {\n", fn_proto(fd, true));
    sb_puts(dst, pre.data);
    sb_puts(dst, out.data);
    if (fd->ret->kind != TY_VOID)
        sb_printf(dst, "    { %s _dead = {0}; return _dead; }\n", c_type(fd->ret));
    sb_puts(dst, "}\n\n");
    sb_free(&pre);
    sb_free(&out);
}

/* ---------------- classes ---------------- */

static FnDecl *vslot_impl(ClassDecl *cd, int slot) {
    for (ClassDecl *k = cd; k; k = k->parent)
        for (int i = 0; i < k->nmethods; i++)
            if (k->methods[i]->vslot == slot) return k->methods[i];
    return NULL;
}

static void emit_class_infra(Em *base, ClassDecl *cd, SBuf *dst) {
    const char *cn = class_cname(cd);

    /* deinit */
    sb_printf(dst, "void %s__deinit(void* vself) {\n", cn);
    sb_printf(dst, "    %s* self = vself; (void)self;\n", cn);
    if (cd->deinit_body) {
        Em em = {0};
        em.mod = base->mod;
        em.aux = base->aux;
        FnDecl dd = {0};
        dd.name = "deinit";
        dd.module = base->mod;
        dd.owner = cd;
        dd.ret = NEW(Type); /* TY_VOID zero value */
        dd.body = cd->deinit_body;
        dd.nbody = cd->ndeinit;
        em.fn = &dd;
        em.cls = cd;
        SBuf pre, out;
        sb_init(&pre);
        sb_init(&out);
        em.pre = &pre;
        em.out = &out;
        em.indent = 1;
        epush(&em, false);
        emit_stmts(&em, cd->deinit_body, cd->ndeinit);
        escope_release(&em, em.scope);
        sb_puts(dst, pre.data);
        sb_puts(dst, out.data);
        sb_free(&pre);
        sb_free(&out);
    }
    for (int i = 0; i < cd->nfields; i++) {
        Field *f = &cd->fields[i];
        if (type_is_ref(f->type) && !f->type->weak)
            sb_printf(dst, "    vt_release(self->f_%s);\n", f->name);
    }
    if (cd->parent)
        sb_printf(dst, "    %s__deinit(vself);\n", class_cname(cd->parent));
    sb_puts(dst, "}\n");

    /* vtable + type */
    if (cd->nvslots > 0) {
        sb_printf(dst, "static void* %s__vtbl[] = {", cn);
        for (int s = 0; s < cd->nvslots; s++) {
            FnDecl *impl = vslot_impl(cd, s);
            sb_printf(dst, "%s(void*)%s", s ? ", " : "", fn_cname(impl));
        }
        sb_puts(dst, "};\n");
    }
    sb_printf(dst, "const VtType %s__type = {\"%s\", %s__deinit, %s, %s};\n\n", cn, cd->name, cn,
              cd->parent ? arena_printf(&g_arena, "&%s__type", class_cname(cd->parent)) : "0",
              cd->nvslots > 0 ? arena_printf(&g_arena, "%s__vtbl", cn) : "0");
}

static void emit_class_new(Em *base, ClassDecl *cd, SBuf *dst) {
    (void)base;
    const char *cn = class_cname(cd);
    FnDecl *ctor = NULL;
    for (ClassDecl *k = cd; k && !ctor; k = k->parent) ctor = k->ctor;
    SBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s* %s__new(", cn, cn);
    if (!ctor || ctor->nparams == 0) sb_puts(&sb, "void");
    else
        for (int i = 0; i < ctor->nparams; i++)
            sb_printf(&sb, "%s%s a%d", i ? ", " : "", c_type(ctor->params[i].type), i);
    sb_puts(&sb, ")");
    sb_printf(dst, "%s {\n", sb.data);
    sb_free(&sb);
    sb_printf(dst, "    %s* self = vt_alloc(sizeof(%s), &%s__type);\n", cn, cn, cn);
    if (ctor) {
        sb_printf(dst, "    %s((%s*)self", fn_cname(ctor), class_cname(ctor->owner));
        for (int i = 0; i < ctor->nparams; i++) sb_printf(dst, ", a%d", i);
        sb_puts(dst, ");\n");
    }
    sb_puts(dst, "    return self;\n}\n\n");
}

/* ---------------- module ---------------- */

static void emit_class_struct(ClassDecl *cd, SBuf *h, bool *emitted, ClassDecl **order, int n) {
    /* emit parent (same module) first */
    for (int i = 0; i < n; i++) {
        if (order[i] == cd) {
            if (emitted[i]) return;
            emitted[i] = true;
        }
    }
    if (cd->parent && cd->parent->module == cd->module)
        emit_class_struct(cd->parent, h, emitted, order, n);
    const char *cn = class_cname(cd);
    sb_printf(h, "struct %s {\n", cn);
    if (cd->parent)
        sb_printf(h, "    %s base;\n", class_cname(cd->parent));
    else
        sb_puts(h, "    VtObj hdr;\n");
    for (int i = 0; i < cd->nfields; i++)
        sb_printf(h, "    %s f_%s;\n", c_type(cd->fields[i].type), cd->fields[i].name);
    sb_puts(h, "};\n");
}

void emit_module(Module *m, bool is_entry, bool checks, bool freestanding, SBuf *h, SBuf *c) {
    g_checks = checks;
    /* ---------- header ---------- */
    sb_printf(h, "#ifndef VOLT_MOD_%s_H\n#define VOLT_MOD_%s_H\n", m->name, m->name);
    sb_puts(h, "#include \"volt_rt.h\"\n");
    for (int i = 0; i < m->ndecls; i++)
        if (m->decls[i]->kind == D_IMPORT)
            sb_printf(h, "#include \"mod_%s.h\"\n", m->decls[i]->import_module->name);
    sb_puts(h, "\n");

    /* struct typedefs + bodies */
    for (int i = 0; i < m->ndecls; i++) {
        if (m->decls[i]->kind != D_STRUCT) continue;
        StructDecl *sd = m->decls[i]->sd;
        const char *sn = struct_cname(sd);
        sb_printf(h, "typedef struct %s %s;\n", sn, sn);
        sb_printf(h, "struct %s {\n", sn);
        for (int f = 0; f < sd->nfields; f++)
            sb_printf(h, "    %s %s%s;\n", c_type(sd->fields[f].type), sd->is_extern ? "" : "f_",
                      sd->fields[f].name);
        sb_puts(h, "};\n");
    }

    /* class forward typedefs */
    int nclasses = 0;
    for (int i = 0; i < m->ndecls; i++)
        if (m->decls[i]->kind == D_CLASS) nclasses++;
    ClassDecl **classes = arena_alloc(&g_arena, sizeof(ClassDecl *) * (size_t)(nclasses ? nclasses : 1));
    bool *emitted = arena_alloc(&g_arena, sizeof(bool) * (size_t)(nclasses ? nclasses : 1));
    int ci = 0;
    for (int i = 0; i < m->ndecls; i++)
        if (m->decls[i]->kind == D_CLASS) classes[ci++] = m->decls[i]->cd;
    for (int i = 0; i < nclasses; i++) {
        const char *cn = class_cname(classes[i]);
        sb_printf(h, "typedef struct %s %s;\n", cn, cn);
    }
    for (int i = 0; i < nclasses; i++)
        emit_class_struct(classes[i], h, emitted, classes, nclasses);
    for (int i = 0; i < nclasses; i++) {
        const char *cn = class_cname(classes[i]);
        sb_printf(h, "extern const VtType %s__type;\n", cn);
        sb_printf(h, "void %s__deinit(void* vself);\n", cn);
        FnDecl *ctor = NULL;
        for (ClassDecl *k = classes[i]; k && !ctor; k = k->parent) ctor = k->ctor;
        sb_printf(h, "%s* %s__new(", cn, cn);
        if (!ctor || ctor->nparams == 0) sb_puts(h, "void");
        else
            for (int p = 0; p < ctor->nparams; p++)
                sb_printf(h, "%s%s", p ? ", " : "", c_type(ctor->params[p].type));
        sb_puts(h, ");\n");
    }
    sb_puts(h, "\n");

    /* extern fns, function prototypes, consts */
    for (int i = 0; i < m->ndecls; i++) {
        Decl *d = m->decls[i];
        switch (d->kind) {
        case D_EXTERN_FN:
            sb_printf(h, "extern %s __asm__(VT_SYM(\"%s\"));\n", fn_proto(d->fn, false),
                      d->fn->name);
            break;
        case D_FN:
            sb_printf(h, "%s;\n", fn_proto(d->fn, false));
            break;
        case D_CLASS: {
            ClassDecl *cd = d->cd;
            if (cd->ctor) sb_printf(h, "%s;\n", fn_proto(cd->ctor, false));
            for (int mi = 0; mi < cd->nmethods; mi++)
                sb_printf(h, "%s;\n", fn_proto(cd->methods[mi], false));
            break;
        }
        case D_STRUCT:
            for (int mi = 0; mi < d->sd->nmethods; mi++)
                sb_printf(h, "%s;\n", fn_proto(d->sd->methods[mi], false));
            break;
        case D_CONST: {
            Expr *e = d->const_init;
            const char *v;
            if (e->kind == EX_INT) v = arena_printf(&g_arena, "%lldLL", (long long)e->ival);
            else if (e->kind == EX_FLOAT) v = arena_printf(&g_arena, "%.17g", e->fval);
            else if (e->kind == EX_BOOL) v = e->ival ? "true" : "false";
            else if (e->kind == EX_NULL) v = "0";
            else if (e->kind == EX_UN && e->lhs->kind == EX_INT)
                v = arena_printf(&g_arena, "-%lldLL", (long long)e->lhs->ival);
            else v = arena_printf(&g_arena, "-%.17g", e->lhs->fval);
            sb_printf(h, "static const %s v_%s_%s = %s;\n", c_type(d->const_type), m->name, d->name,
                      v);
            break;
        }
        default: break;
        }
    }
    sb_puts(h, "#endif\n");

    /* ---------- source ---------- */
    SBuf aux, code;
    sb_init(&aux);
    sb_init(&code);
    Em base = {0};
    base.mod = m;
    base.aux = &aux;

    for (int i = 0; i < m->ndecls; i++) {
        Decl *d = m->decls[i];
        if (d->kind == D_FN) emit_fn(&base, d->fn, NULL, &code);
        else if (d->kind == D_STRUCT) {
            for (int mi = 0; mi < d->sd->nmethods; mi++)
                emit_fn(&base, d->sd->methods[mi], NULL, &code);
        }
        else if (d->kind == D_CLASS) {
            ClassDecl *cd = d->cd;
            if (cd->ctor) emit_fn(&base, cd->ctor, cd, &code);
            for (int mi = 0; mi < cd->nmethods; mi++) emit_fn(&base, cd->methods[mi], cd, &code);
            emit_class_infra(&base, cd, &code);
            emit_class_new(&base, cd, &code);
        }
    }

    sb_printf(c, "#include \"mod_%s.h\"\n\n", m->name);
    sb_puts(c, aux.data);
    sb_puts(c, "\n");
    sb_puts(c, code.data);
    if (is_entry) {
        if (freestanding)
            /* Bare-metal has no argc/argv and owns its own startup: export an
               entry the reset handler / RTOS task calls. Args stay empty. */
            sb_printf(c, "void vt_main(void) {\n    v_%s_main();\n}\n", m->name);
        else
            sb_printf(c, "int main(int argc, char **argv) {\n    vt_set_args(argc, argv);\n"
                         "    v_%s_main();\n    return 0;\n}\n", m->name);
    }
    sb_free(&aux);
    sb_free(&code);
}
