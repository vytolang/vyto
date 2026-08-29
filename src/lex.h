#ifndef VYTO_LEX_H
#define VYTO_LEX_H

#include "util.h"

typedef enum TokKind {
    T_EOF,
    T_IDENT, T_INT, T_FLOAT, T_STRING,
    /* keywords */
    T_FN, T_LET, T_CONST, T_STRUCT, T_CLASS, T_EXTENDS, T_VIRTUAL, T_OVERRIDE,
    T_INIT, T_DEINIT, T_NEW, T_WEAK, T_IMPORT, T_EXPORT, T_FROM, T_EXTERN,
    T_IF, T_ELSE, T_WHILE, T_FOR, T_IN, T_RETURN, T_BREAK, T_CONTINUE,
    T_TRUE, T_FALSE, T_NULL, T_THIS, T_AS, T_SUPER, T_MAP, T_BUILDER, T_ARENA,
    T_SWITCH, T_CASE, T_DEFAULT, T_ENUM, T_STATIC,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_SEMI, T_COLON, T_DOT, T_DOTDOT, T_ARROW, /* => */
    T_HASH, /* # for #link */
    T_AT,   /* @ for new@region */
    T_QUESTION, /* ? for the nullable type suffix T? */
    /* operators */
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_AMPEQ, T_PIPEEQ, T_CARETEQ, T_SHLEQ, T_SHREQ,
    T_OROR, T_ANDAND, T_EQ, T_NEQ, T_LT, T_LE, T_GT, T_GE,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_NOT,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR,
    /* template literals: `chunk{ expr }chunk{ expr }chunk` lexes as a run of
       chunk tokens with the holes lexed as ordinary tokens in between. All four
       carry their chunk bytes in sval/slen, like T_STRING. */
    T_TSTR_START,  /* `chunk{  — opening backtick, a hole follows */
    T_TSTR_MID,    /* }chunk{  — between two holes */
    T_TSTR_END,    /* }chunk`  — final chunk, closing backtick */
    T_TSTR_WHOLE,  /* `chunk`  — a whole template with no holes */
} TokKind;

typedef struct Token {
    TokKind kind;
    Loc loc;
    const char *ident;   /* interned, for T_IDENT and keywords */
    int64_t ival;        /* T_INT */
    double fval;         /* T_FLOAT */
    const char *sval;    /* T_STRING: decoded bytes (arena) */
    size_t slen;         /* T_STRING length (may contain NUL) */
} Token;

/* Nesting cap for templates-inside-holes. It exists so tbrace can be a fixed
   array: the parser snapshots the whole Lexer by value to speculate and rolls
   back by struct assignment (parse.c:264), so every field here must be POD. */
#define VT_MAX_TEMPLATE_DEPTH 8

typedef struct Lexer {
    const char *src;
    const char *p;
    const char *file; /* interned */
    int line, col;
    Token tok;   /* current */
    Token ahead; /* one-token lookahead buffer */
    bool has_ahead;
    /* Template-literal state. tdepth is how many templates are currently open;
       tbrace[d] counts the '{' nesting *inside the current hole* of template d,
       so a block or a nested template in a hole does not end it early. */
    int tdepth;
    int tbrace[VT_MAX_TEMPLATE_DEPTH];
} Lexer;

void lex_init(Lexer *lx, const char *file, const char *src);
void lex_next(Lexer *lx);          /* advance lx->tok */
Token *lex_peek(Lexer *lx);        /* lookahead one token */
const char *tok_desc(TokKind k);   /* for error messages */
const char *tok_keyword(TokKind k); /* the spelling if k is a keyword, else NULL */

#endif
