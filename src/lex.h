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
    T_TRUE, T_FALSE, T_NULL, T_THIS, T_AS, T_SUPER, T_MAP, T_BUILDER,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_SEMI, T_COLON, T_DOT, T_DOTDOT, T_ARROW, /* => */
    T_HASH, /* # for #link */
    /* operators */
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_AMPEQ, T_PIPEEQ, T_CARETEQ, T_SHLEQ, T_SHREQ,
    T_OROR, T_ANDAND, T_EQ, T_NEQ, T_LT, T_LE, T_GT, T_GE,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_NOT,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR,
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

typedef struct Lexer {
    const char *src;
    const char *p;
    const char *file; /* interned */
    int line, col;
    Token tok;   /* current */
    Token ahead; /* one-token lookahead buffer */
    bool has_ahead;
} Lexer;

void lex_init(Lexer *lx, const char *file, const char *src);
void lex_next(Lexer *lx);          /* advance lx->tok */
Token *lex_peek(Lexer *lx);        /* lookahead one token */
const char *tok_desc(TokKind k);   /* for error messages */

#endif
