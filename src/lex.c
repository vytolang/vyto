#include "lex.h"

#include <errno.h>

static const struct { const char *name; TokKind kind; } keywords[] = {
    {"fn", T_FN}, {"let", T_LET}, {"const", T_CONST}, {"struct", T_STRUCT},
    {"class", T_CLASS}, {"extends", T_EXTENDS}, {"virtual", T_VIRTUAL},
    {"override", T_OVERRIDE}, {"init", T_INIT}, {"deinit", T_DEINIT},
    {"new", T_NEW}, {"weak", T_WEAK}, {"import", T_IMPORT}, {"export", T_EXPORT},
    {"from", T_FROM}, {"extern", T_EXTERN}, {"if", T_IF}, {"else", T_ELSE},
    {"while", T_WHILE}, {"for", T_FOR}, {"in", T_IN}, {"return", T_RETURN},
    {"break", T_BREAK}, {"continue", T_CONTINUE}, {"true", T_TRUE},
    {"false", T_FALSE}, {"null", T_NULL}, {"this", T_THIS}, {"as", T_AS},
    {"super", T_SUPER}, {"Map", T_MAP},
};

void lex_init(Lexer *lx, const char *file, const char *src) {
    memset(lx, 0, sizeof *lx);
    lx->src = src;
    lx->p = src;
    lx->file = intern(file);
    lx->line = 1;
    lx->col = 1;
    lex_next(lx);
}

static int lx_getc(Lexer *lx) {
    int c = (unsigned char)*lx->p;
    if (!c) return 0;
    lx->p++;
    if (c == '\n') { lx->line++; lx->col = 1; } else lx->col++;
    return c;
}

static int lx_look(Lexer *lx) { return (unsigned char)*lx->p; }
static int lx_look2(Lexer *lx) { return lx->p[0] ? (unsigned char)lx->p[1] : 0; }

static void skip_ws(Lexer *lx) {
    for (;;) {
        int c = lx_look(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { lx_getc(lx); continue; }
        if (c == '/' && lx_look2(lx) == '/') {
            while (lx_look(lx) && lx_look(lx) != '\n') lx_getc(lx);
            continue;
        }
        if (c == '/' && lx_look2(lx) == '*') {
            Loc start = {lx->file, lx->line, lx->col};
            lx_getc(lx); lx_getc(lx);
            for (;;) {
                if (!lx_look(lx)) fatal_at(start, "unterminated block comment");
                if (lx_look(lx) == '*' && lx_look2(lx) == '/') { lx_getc(lx); lx_getc(lx); break; }
                lx_getc(lx);
            }
            continue;
        }
        break;
    }
}

static bool is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void scan(Lexer *lx, Token *t) {
    skip_ws(lx);
    t->loc.file = lx->file;
    t->loc.line = lx->line;
    t->loc.col = lx->col;

    int c = lx_look(lx);
    if (!c) { t->kind = T_EOF; return; }

    if (is_ident_start(c)) {
        const char *start = lx->p;
        while (is_ident(lx_look(lx))) lx_getc(lx);
        size_t n = (size_t)(lx->p - start);
        t->ident = intern_n(start, n);
        t->kind = T_IDENT;
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
            if (t->ident == intern(keywords[i].name)) { t->kind = keywords[i].kind; break; }
        return;
    }

    if (c >= '0' && c <= '9') {
        const char *start = lx->p;
        if (c == '0' && (lx_look2(lx) == 'x' || lx_look2(lx) == 'X')) {
            lx_getc(lx); lx_getc(lx);
            if (hexval(lx_look(lx)) < 0)
                fatal_at(t->loc, "hex literal needs at least one digit");
            /* accumulate in uint64 so all 64-bit masks work; error past that */
            uint64_t v = 0;
            while (hexval(lx_look(lx)) >= 0) {
                int h = hexval(lx_getc(lx));
                if (v > (UINT64_MAX - (uint64_t)h) / 16)
                    fatal_at(t->loc, "hex literal too large for 64 bits");
                v = v * 16 + (uint64_t)h;
            }
            t->kind = T_INT;
            t->ival = (int64_t)v;
            return;
        }
        while (lx_look(lx) >= '0' && lx_look(lx) <= '9') lx_getc(lx);
        bool isfloat = false;
        if (lx_look(lx) == '.' && lx_look2(lx) >= '0' && lx_look2(lx) <= '9') {
            isfloat = true;
            lx_getc(lx);
            while (lx_look(lx) >= '0' && lx_look(lx) <= '9') lx_getc(lx);
        }
        if (lx_look(lx) == 'e' || lx_look(lx) == 'E') {
            isfloat = true;
            lx_getc(lx);
            if (lx_look(lx) == '+' || lx_look(lx) == '-') lx_getc(lx);
            while (lx_look(lx) >= '0' && lx_look(lx) <= '9') lx_getc(lx);
        }
        char buf[64];
        size_t n = (size_t)(lx->p - start);
        if (n >= sizeof buf) fatal_at(t->loc, "number literal too long");
        memcpy(buf, start, n);
        buf[n] = 0;
        if (isfloat) { t->kind = T_FLOAT; t->fval = strtod(buf, NULL); }
        else {
            errno = 0;
            t->kind = T_INT;
            t->ival = strtoll(buf, NULL, 10);
            if (errno == ERANGE) fatal_at(t->loc, "integer literal too large");
        }
        return;
    }

    if (c == '"') {
        lx_getc(lx);
        SBuf sb;
        sb_init(&sb);
        for (;;) {
            int ch = lx_getc(lx);
            if (!ch || ch == '\n') fatal_at(t->loc, "unterminated string literal");
            if (ch == '"') break;
            if (ch == '\\') {
                int e = lx_getc(lx);
                switch (e) {
                case 'n': sb_putc(&sb, '\n'); break;
                case 't': sb_putc(&sb, '\t'); break;
                case 'r': sb_putc(&sb, '\r'); break;
                case '\\': sb_putc(&sb, '\\'); break;
                case '"': sb_putc(&sb, '"'); break;
                case '0': sb_putc(&sb, '\0'); break;
                case 'x': {
                    int h1 = hexval(lx_getc(lx)), h2 = hexval(lx_getc(lx));
                    if (h1 < 0 || h2 < 0) fatal_at(t->loc, "bad \\x escape");
                    sb_putc(&sb, (char)(h1 * 16 + h2));
                    break;
                }
                default: fatal_at(t->loc, "unknown escape '\\%c'", e);
                }
            } else sb_putc(&sb, (char)ch);
        }
        t->kind = T_STRING;
        t->sval = arena_strndup(&g_arena, sb.data, sb.len);
        t->slen = sb.len;
        sb_free(&sb);
        return;
    }

    lx_getc(lx);
    switch (c) {
    case '(': t->kind = T_LPAREN; return;
    case ')': t->kind = T_RPAREN; return;
    case '{': t->kind = T_LBRACE; return;
    case '}': t->kind = T_RBRACE; return;
    case '[': t->kind = T_LBRACKET; return;
    case ']': t->kind = T_RBRACKET; return;
    case ',': t->kind = T_COMMA; return;
    case ';': t->kind = T_SEMI; return;
    case ':': t->kind = T_COLON; return;
    case '#': t->kind = T_HASH; return;
    case '.':
        if (lx_look(lx) == '.') { lx_getc(lx); t->kind = T_DOTDOT; return; }
        t->kind = T_DOT; return;
    case '=':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_EQ; return; }
        if (lx_look(lx) == '>') { lx_getc(lx); t->kind = T_ARROW; return; }
        t->kind = T_ASSIGN; return;
    case '!':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_NEQ; return; }
        t->kind = T_NOT; return;
    case '<':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_LE; return; }
        if (lx_look(lx) == '<') {
            lx_getc(lx);
            if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_SHLEQ; return; }
            t->kind = T_SHL; return;
        }
        t->kind = T_LT; return;
    case '>':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_GE; return; }
        if (lx_look(lx) == '>') {
            lx_getc(lx);
            if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_SHREQ; return; }
            t->kind = T_SHR; return;
        }
        t->kind = T_GT; return;
    case '+':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_PLUSEQ; return; }
        t->kind = T_PLUS; return;
    case '-':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_MINUSEQ; return; }
        t->kind = T_MINUS; return;
    case '*':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_STAREQ; return; }
        t->kind = T_STAR; return;
    case '/':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_SLASHEQ; return; }
        t->kind = T_SLASH; return;
    case '%':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_PERCENTEQ; return; }
        t->kind = T_PERCENT; return;
    case '&':
        if (lx_look(lx) == '&') { lx_getc(lx); t->kind = T_ANDAND; return; }
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_AMPEQ; return; }
        t->kind = T_AMP; return;
    case '|':
        if (lx_look(lx) == '|') { lx_getc(lx); t->kind = T_OROR; return; }
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_PIPEEQ; return; }
        t->kind = T_PIPE; return;
    case '^':
        if (lx_look(lx) == '=') { lx_getc(lx); t->kind = T_CARETEQ; return; }
        t->kind = T_CARET; return;
    case '~':
        t->kind = T_TILDE; return;
    }
    fatal_at(t->loc, "unexpected character '%c'", c);
}

void lex_next(Lexer *lx) {
    if (lx->has_ahead) {
        lx->tok = lx->ahead;
        lx->has_ahead = false;
        return;
    }
    scan(lx, &lx->tok);
}

Token *lex_peek(Lexer *lx) {
    if (!lx->has_ahead) {
        scan(lx, &lx->ahead);
        lx->has_ahead = true;
    }
    return &lx->ahead;
}

const char *tok_desc(TokKind k) {
    switch (k) {
    case T_EOF: return "end of file";
    case T_IDENT: return "identifier";
    case T_INT: return "integer literal";
    case T_FLOAT: return "float literal";
    case T_STRING: return "string literal";
    case T_LPAREN: return "'('"; case T_RPAREN: return "')'";
    case T_LBRACE: return "'{'"; case T_RBRACE: return "'}'";
    case T_LBRACKET: return "'['"; case T_RBRACKET: return "']'";
    case T_COMMA: return "','"; case T_SEMI: return "';'";
    case T_COLON: return "':'"; case T_DOT: return "'.'";
    case T_DOTDOT: return "'..'"; case T_ARROW: return "'=>'";
    case T_ASSIGN: return "'='"; case T_EQ: return "'=='";
    case T_FN: return "'fn'"; case T_LET: return "'let'";
    default: return "token";
    }
}
