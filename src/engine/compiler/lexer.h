#ifndef HERMES_LEXER_H
#define HERMES_LEXER_H

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,

    TOK_IDENT,
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,

    /* keywords */
    TOK_DEF,
    TOK_IF,
    TOK_ELIF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IN,
    TOK_RANGE,
    TOK_RETURN,
    TOK_IMPORT,
    TOK_PARALLEL,    /* parallel for ... */
    TOK_LOCAL,       /* local <tipo> <nombre> (solo en parallel for) */
    TOK_GLOBAL,      /* global[(op)] <tipo> <nombre> (solo en parallel for) */
    TOK_INSERT_C,    /* insert_C: + indented block of plain C (Token.raw) */
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_TRUE,
    TOK_FALSE,

    /* punctuation / operators */
    TOK_LPAREN, TOK_RPAREN,
    TOK_COLON, TOK_COMMA, TOK_DOT,
    TOK_ARROW,       /* -> */
    TOK_ASSIGN,      /* = */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,

    TOK_UNKNOWN
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start; /* pointer into source, not null-terminated */
    int length;
    int line;
    long int_val;
    double float_val;
    /* TOK_INSERT_C: the block's lines, verbatim (NULL if int_val != 0).
     * int_val: 0 = ok, 1 = no ':' + end of line after insert_C,
     * 2 = no indented block below it. */
    char *raw;
} Token;

typedef struct {
    const char *src;
    int pos;
    int line;
    int paren_depth;

    /* indentation stack */
    int indent_stack[64];
    int indent_top;

    /* pending DEDENT tokens to emit before continuing */
    int pending_dedents;

    int at_line_start;
    int emitted_eof;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);
const char *token_kind_name(TokenKind kind);

#endif
