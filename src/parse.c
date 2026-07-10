#include "parse.h"
#include "lex.h"

typedef struct Parser {
    Lexer lx;
    Module *mod;
} Parser;

/* ---- helpers ---- */

static Loc ploc(Parser *p) { return p->lx.tok.loc; }
static TokKind cur(Parser *p) { return p->lx.tok.kind; }
static void advance(Parser *p) { lex_next(&p->lx); }

static bool accept(Parser *p, TokKind k) {
    if (cur(p) == k) { advance(p); return true; }
    return false;
}

static void expect(Parser *p, TokKind k) {
    if (cur(p) != k)
        fatal_at(ploc(p), "expected %s, got %s", tok_desc(k), tok_desc(cur(p)));
    advance(p);
}

/* close a Map<...>: a '>>' or '>>=' left by nested generics splits into '>' + rest */
static void expect_gt(Parser *p) {
    if (cur(p) == T_SHR) { p->lx.tok.kind = T_GT; return; }
    if (cur(p) == T_SHREQ) { p->lx.tok.kind = T_GE; return; }
    expect(p, T_GT);
}

static const char *expect_ident(Parser *p) {
    if (cur(p) != T_IDENT)
        fatal_at(ploc(p), "expected identifier, got %s", tok_desc(cur(p)));
    const char *s = p->lx.tok.ident;
    advance(p);
    return s;
}

/* growable pointer-array helper (arena-backed, geometric growth) */
typedef struct PtrVec { void **items; int len, cap; } PtrVec;
static void pv_push(PtrVec *v, void *item) {
    if (v->len == v->cap) {
        int ncap = v->cap ? v->cap * 2 : 8;
        void **ni = arena_alloc(&g_arena, sizeof(void *) * (size_t)ncap);
        memcpy(ni, v->items, sizeof(void *) * (size_t)v->len);
        v->items = ni;
        v->cap = ncap;
    }
    v->items[v->len++] = item;
}

static Expr *new_expr(Parser *p, ExprKind k) {
    Expr *e = NEW(Expr);
    e->kind = k;
    e->loc = ploc(p);
    return e;
}

static Stmt *new_stmt(Parser *p, StmtKind k) {
    Stmt *s = NEW(Stmt);
    s->kind = k;
    s->loc = ploc(p);
    return s;
}

/* ---- types ---- */

static Type *type_new(TypeKind k) {
    Type *t = NEW(Type);
    t->kind = k;
    return t;
}

static Type *parse_type(Parser *p);

static Type *parse_base_type(Parser *p) {
    Loc loc = ploc(p);
    Type *t;
    if (accept(p, T_WEAK)) {
        t = parse_base_type(p);
        t->weak = true;
        return t;
    }
    if (accept(p, T_MAP)) {
        expect(p, T_LT);
        Type *kt = parse_type(p);
        if (!(kt->kind == TY_NAMED && kt->name == intern("string")) && kt->kind != TY_STRING)
            fatal_at(loc, "map keys must be 'string' in v0.1");
        expect(p, T_COMMA);
        Type *vt = parse_type(p);
        expect_gt(p);
        t = type_new(TY_MAP);
        t->elem = vt;
        t->loc = loc;
        return t;
    }
    if (accept(p, T_FN)) {
        expect(p, T_LPAREN);
        PtrVec params = {0};
        while (cur(p) != T_RPAREN) {
            pv_push(&params, parse_type(p));
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RPAREN);
        t = type_new(TY_FN);
        t->params = (Type **)params.items;
        t->nparams = params.len;
        t->ret = accept(p, T_COLON) ? parse_type(p) : type_new(TY_VOID);
        t->loc = loc;
        return t;
    }
    const char *name = expect_ident(p);
    static const struct { const char *n; TypeKind k; } prims[] = {
        {"int", TY_INT}, {"float", TY_FLOAT}, {"bool", TY_BOOL}, {"byte", TY_BYTE},
        {"string", TY_STRING}, {"cstring", TY_CSTRING}, {"rawptr", TY_RAWPTR},
        {"void", TY_VOID},
        {"i8", TY_I8}, {"i16", TY_I16}, {"i32", TY_I32}, {"i64", TY_I64},
        {"u8", TY_U8}, {"u16", TY_U16}, {"u32", TY_U32}, {"u64", TY_U64},
        {"f32", TY_F32}, {"f64", TY_F64},
        {"clong", TY_CLONG}, {"culong", TY_CULONG},
    };
    for (size_t i = 0; i < sizeof prims / sizeof prims[0]; i++) {
        if (name == intern(prims[i].n)) {
            t = type_new(prims[i].k);
            t->loc = loc;
            return t;
        }
    }
    t = type_new(TY_NAMED);
    t->name = name;
    t->loc = loc;
    return t;
}

static Type *parse_type(Parser *p) {
    Type *t = parse_base_type(p);
    while (cur(p) == T_LBRACKET && lex_peek(&p->lx)->kind == T_RBRACKET) {
        advance(p);
        advance(p);
        Type *arr = type_new(TY_ARRAY);
        arr->elem = t;
        arr->loc = t->loc;
        t = arr;
    }
    return t;
}

/* ---- expressions ---- */

static Expr *parse_expr(Parser *p);
static Stmt **parse_block(Parser *p, int *count);

/* at "ident :" (a named-argument label), not "ident" alone? */
static bool at_arg_label(Parser *p) {
    if (cur(p) != T_IDENT) return false;
    Lexer save = p->lx; /* lexer is a value type; safe to snapshot */
    advance(p);
    bool is_label = cur(p) == T_COLON;
    p->lx = save;
    return is_label;
}

static Expr **parse_args_named(Parser *p, int *nargs, const char ***arg_names) {
    expect(p, T_LPAREN);
    PtrVec args = {0}, names = {0};
    bool any_named = false;
    while (cur(p) != T_RPAREN) {
        const char *label = NULL;
        if (at_arg_label(p)) {
            label = p->lx.tok.ident;
            advance(p);       /* ident */
            advance(p);       /* ':'   */
            any_named = true;
        }
        pv_push(&names, (void *)label);
        pv_push(&args, parse_expr(p));
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN);
    *nargs = args.len;
    *arg_names = any_named ? (const char **)names.items : NULL;
    return (Expr **)args.items;
}

static Expr **parse_args(Parser *p, int *nargs) {
    const char **names = NULL;
    return parse_args_named(p, nargs, &names);
}

/* speculative check: are we at "(id, id, ...) =>" ? */
static bool looks_like_arrow(Parser *p) {
    if (cur(p) != T_LPAREN) return false;
    Lexer save = p->lx; /* lexer is a value type; safe to snapshot */
    bool result = false;
    advance(p);
    if (cur(p) == T_RPAREN) {
        advance(p);
        result = cur(p) == T_ARROW;
    } else {
        for (;;) {
            if (cur(p) != T_IDENT) break;
            advance(p);
            if (accept(p, T_COMMA)) continue;
            if (cur(p) == T_RPAREN) {
                advance(p);
                result = cur(p) == T_ARROW;
            }
            break;
        }
    }
    p->lx = save;
    return result;
}

static Expr *parse_arrow(Parser *p) {
    Expr *e = new_expr(p, EX_ARROW);
    FnDecl *fd = NEW(FnDecl);
    fd->loc = ploc(p);
    fd->module = p->mod;
    fd->vslot = -1;
    fd->arrow_id = ++p->mod->arrow_counter;
    expect(p, T_LPAREN);
    PtrVec params = {0};
    while (cur(p) != T_RPAREN) {
        Param *pa = NEW(Param);
        pa->loc = ploc(p);
        pa->name = expect_ident(p);
        pa->type = NULL; /* inferred from context by checker */
        pv_push(&params, pa);
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN);
    fd->nparams = params.len;
    fd->params = arena_alloc(&g_arena, sizeof(Param) * (size_t)(params.len ? params.len : 1));
    for (int i = 0; i < params.len; i++) fd->params[i] = *(Param *)params.items[i];
    expect(p, T_ARROW);
    if (cur(p) == T_LBRACE) {
        fd->body = parse_block(p, &fd->nbody);
    } else {
        /* expression body: sugar for { return expr; } / { expr; } — checker decides */
        Stmt *s = new_stmt(p, ST_RETURN);
        s->expr = parse_expr(p);
        PtrVec body = {0};
        pv_push(&body, s);
        fd->body = (Stmt **)body.items;
        fd->nbody = 1;
    }
    e->fn_lit = fd;
    return e;
}

static Expr *parse_primary(Parser *p) {
    Expr *e;
    switch (cur(p)) {
    case T_INT:
        e = new_expr(p, EX_INT);
        e->ival = p->lx.tok.ival;
        advance(p);
        return e;
    case T_FLOAT:
        e = new_expr(p, EX_FLOAT);
        e->fval = p->lx.tok.fval;
        advance(p);
        return e;
    case T_STRING:
        e = new_expr(p, EX_STR);
        e->sval = p->lx.tok.sval;
        e->slen = p->lx.tok.slen;
        advance(p);
        return e;
    case T_TRUE: case T_FALSE:
        e = new_expr(p, EX_BOOL);
        e->ival = cur(p) == T_TRUE;
        advance(p);
        return e;
    case T_NULL:
        e = new_expr(p, EX_NULL);
        advance(p);
        return e;
    case T_THIS:
        e = new_expr(p, EX_THIS);
        advance(p);
        return e;
    case T_SUPER: {
        e = new_expr(p, EX_CALL);
        advance(p);
        expect(p, T_DOT);
        if (!accept(p, T_INIT)) fatal_at(e->loc, "only 'super.init(...)' is supported");
        e->is_super_call = true;
        e->name = intern("init");
        e->args = parse_args(p, &e->nargs);
        return e;
    }
    case T_NEW: {
        e = new_expr(p, EX_NEW);
        advance(p);
        if (cur(p) == T_MAP) {
            e->cast_type = parse_type(p); /* Map<string,T> */
            int n;
            Expr **a = parse_args(p, &n);
            (void)a;
            if (n != 0) fatal_at(e->loc, "new Map takes no arguments");
            return e;
        }
        e->name = expect_ident(p);
        e->args = parse_args_named(p, &e->nargs, &e->arg_names);
        return e;
    }
    case T_IDENT:
        e = new_expr(p, EX_IDENT);
        e->name = p->lx.tok.ident;
        advance(p);
        return e;
    case T_LBRACKET: {
        e = new_expr(p, EX_ARRAYLIT);
        advance(p);
        PtrVec elems = {0};
        while (cur(p) != T_RBRACKET) {
            pv_push(&elems, parse_expr(p));
            if (!accept(p, T_COMMA)) break;
        }
        expect(p, T_RBRACKET);
        e->args = (Expr **)elems.items;
        e->nargs = elems.len;
        return e;
    }
    case T_LPAREN:
        if (looks_like_arrow(p)) return parse_arrow(p);
        advance(p);
        e = parse_expr(p);
        expect(p, T_RPAREN);
        return e;
    default:
        fatal_at(ploc(p), "expected expression, got %s", tok_desc(cur(p)));
        return NULL;
    }
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        if (cur(p) == T_LPAREN) {
            Expr *call = new_expr(p, EX_CALL);
            call->lhs = e;
            call->args = parse_args_named(p, &call->nargs, &call->arg_names);
            e = call;
        } else if (accept(p, T_LBRACKET)) {
            Expr *ix = new_expr(p, EX_INDEX);
            ix->lhs = e;
            ix->rhs = parse_expr(p);
            expect(p, T_RBRACKET);
            e = ix;
        } else if (accept(p, T_DOT)) {
            Expr *m = new_expr(p, EX_MEMBER);
            m->lhs = e;
            if (cur(p) == T_INIT) { m->name = intern("init"); advance(p); }
            else m->name = expect_ident(p);
            e = m;
        } else break;
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    if (cur(p) == T_NOT || cur(p) == T_MINUS || cur(p) == T_TILDE || cur(p) == T_AMP) {
        Expr *e = new_expr(p, EX_UN);
        e->op = cur(p);
        advance(p);
        e->lhs = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

static Expr *parse_as(Parser *p) {
    Expr *e = parse_unary(p);
    while (cur(p) == T_AS) {
        Expr *c = new_expr(p, EX_AS);
        advance(p);
        c->lhs = e;
        c->cast_type = parse_type(p);
        e = c;
    }
    return e;
}

static int bin_prec(TokKind k) {
    switch (k) {
    /* Go-style: shifts and '&' bind like '*', '|' and '^' like '+' —
       so `a & mask == 0` parses as `(a & mask) == 0` */
    case T_STAR: case T_SLASH: case T_PERCENT:
    case T_SHL: case T_SHR: case T_AMP: return 6;
    case T_PLUS: case T_MINUS: case T_PIPE: case T_CARET: return 5;
    case T_LT: case T_LE: case T_GT: case T_GE: return 4;
    case T_EQ: case T_NEQ: return 3;
    case T_ANDAND: return 2;
    case T_OROR: return 1;
    default: return 0;
    }
}

static Expr *parse_bin(Parser *p, int min_prec) {
    Expr *lhs = parse_as(p);
    for (;;) {
        int prec = bin_prec(cur(p));
        if (prec < min_prec || prec == 0) return lhs;
        Expr *e = new_expr(p, EX_BIN);
        e->op = cur(p);
        advance(p);
        e->lhs = lhs;
        e->rhs = parse_bin(p, prec + 1);
        lhs = e;
    }
}

static bool is_assign_op(TokKind k) {
    return k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ || k == T_STAREQ ||
           k == T_SLASHEQ || k == T_PERCENTEQ || k == T_AMPEQ || k == T_PIPEEQ ||
           k == T_CARETEQ || k == T_SHLEQ || k == T_SHREQ;
}

static Expr *parse_expr(Parser *p) {
    Expr *lhs = parse_bin(p, 1);
    if (is_assign_op(cur(p))) {
        Expr *e = new_expr(p, EX_ASSIGN);
        e->op = cur(p);
        advance(p);
        e->lhs = lhs;
        e->rhs = parse_expr(p); /* right-assoc */
        return e;
    }
    return lhs;
}

/* ---- statements ---- */

static Stmt *parse_stmt(Parser *p);

static Stmt **parse_block(Parser *p, int *count) {
    expect(p, T_LBRACE);
    PtrVec stmts = {0};
    while (cur(p) != T_RBRACE) pv_push(&stmts, parse_stmt(p));
    expect(p, T_RBRACE);
    *count = stmts.len;
    return (Stmt **)stmts.items;
}

static Stmt *parse_stmt(Parser *p) {
    Stmt *s;
    switch (cur(p)) {
    case T_LET:
        s = new_stmt(p, ST_LET);
        advance(p);
        s->name = expect_ident(p);
        if (accept(p, T_COLON)) s->decl_type = parse_type(p);
        expect(p, T_ASSIGN);
        s->init = parse_expr(p);
        expect(p, T_SEMI);
        return s;
    case T_IF: {
        s = new_stmt(p, ST_IF);
        advance(p);
        expect(p, T_LPAREN);
        s->expr = parse_expr(p);
        expect(p, T_RPAREN);
        s->body = parse_block(p, &s->nbody);
        if (accept(p, T_ELSE)) {
            if (cur(p) == T_IF) {
                PtrVec els = {0};
                pv_push(&els, parse_stmt(p));
                s->els = (Stmt **)els.items;
                s->nels = 1;
            } else {
                s->els = parse_block(p, &s->nels);
            }
        }
        return s;
    }
    case T_WHILE:
        s = new_stmt(p, ST_WHILE);
        advance(p);
        expect(p, T_LPAREN);
        s->expr = parse_expr(p);
        expect(p, T_RPAREN);
        s->body = parse_block(p, &s->nbody);
        return s;
    case T_FOR: {
        s = new_stmt(p, ST_FOR_EACH);
        advance(p);
        expect(p, T_LPAREN);
        expect(p, T_LET);
        s->name = expect_ident(p);
        expect(p, T_IN);
        Expr *first = parse_expr(p);
        if (accept(p, T_DOTDOT)) {
            s->kind = ST_FOR_RANGE;
            s->range_lo = first;
            s->range_hi = parse_expr(p);
        } else {
            s->iter = first;
        }
        expect(p, T_RPAREN);
        s->body = parse_block(p, &s->nbody);
        return s;
    }
    case T_RETURN:
        s = new_stmt(p, ST_RETURN);
        advance(p);
        if (cur(p) != T_SEMI) s->expr = parse_expr(p);
        expect(p, T_SEMI);
        return s;
    case T_BREAK:
        s = new_stmt(p, ST_BREAK);
        advance(p);
        expect(p, T_SEMI);
        return s;
    case T_CONTINUE:
        s = new_stmt(p, ST_CONTINUE);
        advance(p);
        expect(p, T_SEMI);
        return s;
    case T_LBRACE:
        s = new_stmt(p, ST_BLOCK);
        s->body = parse_block(p, &s->nbody);
        return s;
    default:
        s = new_stmt(p, ST_EXPR);
        s->expr = parse_expr(p);
        expect(p, T_SEMI);
        return s;
    }
}

/* ---- declarations ---- */

static Param *parse_params(Parser *p, int *nparams) {
    expect(p, T_LPAREN);
    PtrVec params = {0};
    bool seen_default = false;
    while (cur(p) != T_RPAREN) {
        Param *pa = NEW(Param);
        pa->loc = ploc(p);
        pa->name = expect_ident(p);
        expect(p, T_COLON);
        pa->type = parse_type(p);
        if (accept(p, T_ASSIGN)) {
            pa->def = parse_expr(p);
            seen_default = true;
        } else if (seen_default) {
            fatal_at(pa->loc, "non-default parameter '%s' follows a default parameter", pa->name);
        }
        pv_push(&params, pa);
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RPAREN);
    *nparams = params.len;
    Param *out = arena_alloc(&g_arena, sizeof(Param) * (size_t)(params.len ? params.len : 1));
    for (int i = 0; i < params.len; i++) out[i] = *(Param *)params.items[i];
    return out;
}

static FnDecl *parse_fn(Parser *p, bool is_extern) {
    FnDecl *fd = NEW(FnDecl);
    fd->loc = ploc(p);
    fd->module = p->mod;
    fd->vslot = -1;
    expect(p, T_FN);
    fd->name = expect_ident(p);
    fd->params = parse_params(p, &fd->nparams);
    fd->ret = accept(p, T_COLON) ? parse_type(p) : type_new(TY_VOID);
    if (is_extern) {
        fd->is_extern = true;
        for (int i = 0; i < fd->nparams; i++)
            if (fd->params[i].def)
                fatal_at(fd->params[i].loc, "extern functions cannot have default arguments");
        expect(p, T_SEMI);
    } else {
        fd->body = parse_block(p, &fd->nbody);
    }
    return fd;
}

static StructDecl *parse_struct(Parser *p, bool is_extern) {
    StructDecl *sd = NEW(StructDecl);
    sd->loc = ploc(p);
    sd->module = p->mod;
    sd->is_extern = is_extern;
    expect(p, T_STRUCT);
    sd->name = expect_ident(p);
    expect(p, T_LBRACE);
    PtrVec fields = {0}, methods = {0};
    while (cur(p) != T_RBRACE) {
        if (cur(p) == T_BUILDER || cur(p) == T_VIRTUAL || cur(p) == T_OVERRIDE)
            fatal_at(ploc(p), "struct methods cannot be virtual/override/builder");
        if (cur(p) == T_DEINIT) fatal_at(ploc(p), "structs cannot have deinit");
        if (cur(p) == T_FN) {
            FnDecl *m = NEW(FnDecl);
            m->loc = ploc(p);
            m->module = p->mod;
            m->sowner = sd;
            m->vslot = -1;
            advance(p);
            if (cur(p) == T_INIT) fatal_at(ploc(p), "structs have no init; construct with S(...)");
            m->name = expect_ident(p);
            m->params = parse_params(p, &m->nparams);
            m->ret = accept(p, T_COLON) ? parse_type(p) : type_new(TY_VOID);
            m->body = parse_block(p, &m->nbody);
            pv_push(&methods, m);
            continue;
        }
        Field *f = NEW(Field);
        f->loc = ploc(p);
        f->name = expect_ident(p);
        expect(p, T_COLON);
        f->type = parse_type(p);
        expect(p, T_SEMI);
        pv_push(&fields, f);
    }
    expect(p, T_RBRACE);
    sd->nfields = fields.len;
    sd->fields = arena_alloc(&g_arena, sizeof(Field) * (size_t)(fields.len ? fields.len : 1));
    for (int i = 0; i < fields.len; i++) sd->fields[i] = *(Field *)fields.items[i];
    sd->nmethods = methods.len;
    sd->methods = (FnDecl **)methods.items;
    return sd;
}

static ClassDecl *parse_class(Parser *p) {
    ClassDecl *cd = NEW(ClassDecl);
    cd->loc = ploc(p);
    cd->module = p->mod;
    expect(p, T_CLASS);
    cd->name = expect_ident(p);
    if (accept(p, T_EXTENDS)) cd->parent_name = expect_ident(p);
    expect(p, T_LBRACE);
    PtrVec fields = {0}, methods = {0};
    while (cur(p) != T_RBRACE) {
        bool is_builder = accept(p, T_BUILDER);
        bool is_virtual = accept(p, T_VIRTUAL);
        bool is_override = !is_virtual && accept(p, T_OVERRIDE);
        if (is_builder && (is_virtual || is_override))
            fatal_at(ploc(p), "builder methods cannot be virtual/override");
        if (cur(p) == T_DEINIT) {
            if (is_builder) fatal_at(ploc(p), "deinit cannot be a builder");
            if (is_virtual || is_override) fatal_at(ploc(p), "deinit cannot be virtual/override");
            if (cd->deinit_body) fatal_at(ploc(p), "duplicate deinit");
            advance(p);
            cd->deinit_body = parse_block(p, &cd->ndeinit);
            continue;
        }
        if (cur(p) == T_FN) {
            FnDecl *m = NEW(FnDecl);
            m->loc = ploc(p);
            m->module = p->mod;
            m->owner = cd;
            m->vslot = -1;
            m->is_virtual = is_virtual;
            m->is_override = is_override;
            m->is_builder = is_builder;
            advance(p);
            if (cur(p) == T_INIT) {
                if (is_builder) fatal_at(ploc(p), "init cannot be a builder");
                if (is_virtual || is_override) fatal_at(ploc(p), "init cannot be virtual/override");
                advance(p);
                m->name = intern("init");
                m->params = parse_params(p, &m->nparams);
                m->ret = type_new(TY_VOID);
                m->body = parse_block(p, &m->nbody);
                if (cd->ctor) fatal_at(m->loc, "duplicate init");
                cd->ctor = m;
                continue;
            }
            m->name = expect_ident(p);
            m->params = parse_params(p, &m->nparams);
            if (is_builder) {
                if (cur(p) == T_COLON)
                    fatal_at(ploc(p), "builder methods cannot declare a return type (they return the receiver)");
                Type *rt = type_new(TY_NAMED);
                rt->name = cd->name;
                rt->loc = m->loc;
                m->ret = rt;
            } else {
                m->ret = accept(p, T_COLON) ? parse_type(p) : type_new(TY_VOID);
            }
            m->body = parse_block(p, &m->nbody);
            pv_push(&methods, m);
            continue;
        }
        if (is_virtual || is_override) fatal_at(ploc(p), "expected 'fn' after virtual/override");
        /* field */
        Field *f = NEW(Field);
        f->loc = ploc(p);
        f->name = expect_ident(p);
        expect(p, T_COLON);
        f->type = parse_type(p);
        expect(p, T_SEMI);
        pv_push(&fields, f);
    }
    expect(p, T_RBRACE);
    cd->nfields = fields.len;
    cd->fields = arena_alloc(&g_arena, sizeof(Field) * (size_t)(fields.len ? fields.len : 1));
    for (int i = 0; i < fields.len; i++) cd->fields[i] = *(Field *)fields.items[i];
    cd->nmethods = methods.len;
    cd->methods = (FnDecl **)methods.items;
    return cd;
}

static Decl *new_decl(Parser *p, DeclKind k) {
    Decl *d = NEW(Decl);
    d->kind = k;
    d->loc = ploc(p);
    return d;
}

Module *parse_module(const char *path, const char *modname, const char *src) {
    Parser p;
    p.mod = NEW(Module);
    p.mod->name = intern(modname);
    p.mod->path = intern(path);
    p.mod->src = src;
    p.mod->src_hash = fnv1a(src, strlen(src));
    lex_init(&p.lx, path, src);

    PtrVec decls = {0};
    while (cur(&p) != T_EOF) {
        accept(&p, T_EXPORT); /* v0.1: everything is exported */
        Decl *d;
        switch (cur(&p)) {
        case T_HASH: {
            d = new_decl(&p, D_LINK);
            advance(&p);
            const char *kw = expect_ident(&p);
            if (kw != intern("link")) fatal_at(d->loc, "unknown pragma '#%s'", kw);
            if (cur(&p) != T_STRING) fatal_at(ploc(&p), "expected library name string after #link");
            d->link_lib = arena_strndup(&g_arena, p.lx.tok.sval, p.lx.tok.slen);
            advance(&p);
            /* optional platform condition: #link "X11" if "linux" —
               the library is linked only when the target triple starts
               with the given OS prefix (linux / macos / windows) */
            if (accept(&p, T_IF)) {
                if (cur(&p) != T_STRING)
                    fatal_at(ploc(&p), "expected platform string after 'if'");
                d->link_cond = arena_strndup(&g_arena, p.lx.tok.sval, p.lx.tok.slen);
                advance(&p);
            }
            break;
        }
        case T_IMPORT: {
            d = new_decl(&p, D_IMPORT);
            advance(&p);
            expect(&p, T_LBRACE);
            PtrVec names = {0};
            while (cur(&p) != T_RBRACE) {
                pv_push(&names, (void *)expect_ident(&p));
                if (!accept(&p, T_COMMA)) break;
            }
            expect(&p, T_RBRACE);
            expect(&p, T_FROM);
            if (cur(&p) != T_STRING) fatal_at(ploc(&p), "expected module path string");
            d->import_path = arena_strndup(&g_arena, p.lx.tok.sval, p.lx.tok.slen);
            advance(&p);
            expect(&p, T_SEMI);
            d->import_names = (const char **)names.items;
            d->nimport_names = names.len;
            break;
        }
        case T_CONST: {
            d = new_decl(&p, D_CONST);
            advance(&p);
            d->name = expect_ident(&p);
            expect(&p, T_COLON);
            d->const_type = parse_type(&p);
            expect(&p, T_ASSIGN);
            d->const_init = parse_expr(&p);
            expect(&p, T_SEMI);
            break;
        }
        case T_FN:
            d = new_decl(&p, D_FN);
            d->fn = parse_fn(&p, false);
            d->name = d->fn->name;
            break;
        case T_STRUCT:
            d = new_decl(&p, D_STRUCT);
            d->sd = parse_struct(&p, false);
            d->name = d->sd->name;
            break;
        case T_CLASS:
            d = new_decl(&p, D_CLASS);
            d->cd = parse_class(&p);
            d->name = d->cd->name;
            break;
        case T_EXTERN: {
            advance(&p);
            if (cur(&p) != T_STRING || p.lx.tok.slen != 1 || p.lx.tok.sval[0] != 'C')
                fatal_at(ploc(&p), "expected \"C\" after extern");
            advance(&p);
            expect(&p, T_LBRACE);
            while (cur(&p) != T_RBRACE) {
                Decl *ed;
                if (cur(&p) == T_FN) {
                    ed = new_decl(&p, D_EXTERN_FN);
                    ed->fn = parse_fn(&p, true);
                    ed->name = ed->fn->name;
                } else if (cur(&p) == T_STRUCT) {
                    ed = new_decl(&p, D_STRUCT);
                    ed->sd = parse_struct(&p, true);
                    ed->name = ed->sd->name;
                } else {
                    fatal_at(ploc(&p), "expected fn or struct in extern block");
                }
                pv_push(&decls, ed);
            }
            expect(&p, T_RBRACE);
            continue; /* decls already pushed */
        }
        default:
            fatal_at(ploc(&p), "expected declaration, got %s", tok_desc(cur(&p)));
            d = NULL;
        }
        pv_push(&decls, d);
    }
    p.mod->decls = (Decl **)decls.items;
    p.mod->ndecls = decls.len;
    for (int i = 0; i < p.mod->ndecls; i++) p.mod->decls[i]->module = p.mod;
    return p.mod;
}
