#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

typedef struct { const char *word; TokenKind kind; } Keyword;

static const Keyword KEYWORDS[] = {
    {"def", TOK_DEF}, {"if", TOK_IF}, {"elif", TOK_ELIF}, {"else", TOK_ELSE},
    {"while", TOK_WHILE}, {"for", TOK_FOR}, {"in", TOK_IN}, {"range", TOK_RANGE},
    {"return", TOK_RETURN}, {"import", TOK_IMPORT},
    {"and", TOK_AND}, {"or", TOK_OR}, {"not", TOK_NOT},
    {"True", TOK_TRUE}, {"False", TOK_FALSE},
};
#define NUM_KEYWORDS (int)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->paren_depth = 0;
    lx->indent_stack[0] = 0;
    lx->indent_top = 0;
    lx->pending_dedents = 0;
    lx->at_line_start = 1;
    lx->emitted_eof = 0;
}

static char peekc(Lexer *lx) { return lx->src[lx->pos]; }
static char peekc_at(Lexer *lx, int off) { return lx->src[lx->pos + off]; }

static Token make_tok(Lexer *lx, TokenKind kind, const char *start, int length) {
    Token t;
    t.kind = kind;
    t.start = start;
    t.length = length;
    t.line = lx->line;
    t.int_val = 0;
    t.float_val = 0;
    return t;
}

/* Scans leading whitespace of the current line and resolves INDENT/DEDENT
 * bookkeeping. Returns 1 if an INDENT or DEDENT token was produced into *out,
 * 0 if the caller should fall through to normal tokenizing on this line,
 * and -1 if we hit EOF while at line start. */
static int handle_line_start(Lexer *lx, Token *out) {
    for (;;) {
        int line_begin = lx->pos;
        int width = 0;
        while (peekc(lx) == ' ') { width++; lx->pos++; }
        if (peekc(lx) == '\t') {
            /* tabs not supported in v0.1: treat as 8 spaces each */
            while (peekc(lx) == '\t') { width += 8; lx->pos++; }
        }

        char c = peekc(lx);
        if (c == '\0') {
            lx->pos = line_begin; /* restore, EOF handled by caller */
            if (lx->indent_top > 0) {
                lx->pending_dedents = lx->indent_top - 1;
                lx->indent_top = 0;
                *out = make_tok(lx, TOK_DEDENT, lx->src + lx->pos, 0);
                return 1;
            }
            return -1;
        }
        if (c == '\n') { lx->pos++; lx->line++; continue; }      /* blank line */
        if (c == '#') { while (peekc(lx) != '\n' && peekc(lx) != '\0') lx->pos++; continue; }

        /* real line of code found */
        if (width > lx->indent_stack[lx->indent_top]) {
            lx->indent_top++;
            lx->indent_stack[lx->indent_top] = width;
            lx->at_line_start = 0;
            *out = make_tok(lx, TOK_INDENT, lx->src + lx->pos, 0);
            return 1;
        } else if (width < lx->indent_stack[lx->indent_top]) {
            int popped = 0;
            while (lx->indent_top > 0 && width < lx->indent_stack[lx->indent_top]) {
                lx->indent_top--;
                popped++;
            }
            if (width > lx->indent_stack[lx->indent_top]) {
                /* inconsistent indentation: adopt it as a new level (lenient) */
                lx->indent_top++;
                lx->indent_stack[lx->indent_top] = width;
            }
            lx->pending_dedents = popped - 1;
            lx->at_line_start = 0;
            *out = make_tok(lx, TOK_DEDENT, lx->src + lx->pos, 0);
            return 1;
        } else {
            lx->at_line_start = 0;
            return 0; /* same indentation, tokenize normally */
        }
    }
}

static TokenKind ident_keyword_kind(const char *start, int length) {
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        size_t klen = strlen(KEYWORDS[i].word);
        if ((int)klen == length && strncmp(KEYWORDS[i].word, start, length) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TOK_IDENT;
}

Token lexer_next(Lexer *lx) {
    if (lx->pending_dedents > 0) {
        lx->pending_dedents--;
        return make_tok(lx, TOK_DEDENT, lx->src + lx->pos, 0);
    }

    if (lx->at_line_start) {
        Token out;
        int r = handle_line_start(lx, &out);
        if (r == 1) return out;
        if (r == -1) {
            if (!lx->emitted_eof) { lx->emitted_eof = 1; return make_tok(lx, TOK_EOF, lx->src + lx->pos, 0); }
            return make_tok(lx, TOK_EOF, lx->src + lx->pos, 0);
        }
    }

    /* skip intra-line whitespace and comments */
    for (;;) {
        char c = peekc(lx);
        if (c == ' ' || c == '\t') { lx->pos++; continue; }
        if (c == '#') { while (peekc(lx) != '\n' && peekc(lx) != '\0') lx->pos++; continue; }
        break;
    }

    char c = peekc(lx);

    if (c == '\0') {
        if (lx->indent_top > 0) {
            lx->pending_dedents = lx->indent_top - 1;
            lx->indent_top = 0;
            return make_tok(lx, TOK_DEDENT, lx->src + lx->pos, 0);
        }
        lx->emitted_eof = 1;
        return make_tok(lx, TOK_EOF, lx->src + lx->pos, 0);
    }

    if (c == '\n') {
        lx->pos++;
        int prev_line = lx->line;
        lx->line++;
        if (lx->paren_depth > 0) {
            return lexer_next(lx); /* implicit continuation inside parens */
        }
        lx->at_line_start = 1;
        Token t = make_tok(lx, TOK_NEWLINE, lx->src + lx->pos - 1, 1);
        t.line = prev_line;
        return t;
    }

    const char *start = lx->src + lx->pos;

    if (isalpha((unsigned char)c) || c == '_') {
        int len = 0;
        while (isalnum((unsigned char)peekc(lx)) || peekc(lx) == '_') { lx->pos++; len++; }
        TokenKind kind = ident_keyword_kind(start, len);
        return make_tok(lx, kind, start, len);
    }

    if (isdigit((unsigned char)c)) {
        int len = 0;
        int is_float = 0;
        while (isdigit((unsigned char)peekc(lx))) { lx->pos++; len++; }
        if (peekc(lx) == '.' && isdigit((unsigned char)peekc_at(lx, 1))) {
            is_float = 1;
            lx->pos++; len++;
            while (isdigit((unsigned char)peekc(lx))) { lx->pos++; len++; }
        }
        Token t = make_tok(lx, is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, start, len);
        if (is_float) t.float_val = strtod(start, NULL);
        else t.int_val = strtol(start, NULL, 10);
        return t;
    }

    if (c == '"') {
        lx->pos++;
        int len = 0;
        const char *str_start = lx->src + lx->pos;
        while (peekc(lx) != '"' && peekc(lx) != '\0') {
            if (peekc(lx) == '\\' && peekc_at(lx, 1) != '\0') { lx->pos++; len++; }
            lx->pos++; len++;
        }
        if (peekc(lx) == '"') lx->pos++;
        return make_tok(lx, TOK_STRING_LIT, str_start, len);
    }

    lx->pos++;
    switch (c) {
        case '(' : lx->paren_depth++; return make_tok(lx, TOK_LPAREN, start, 1);
        case ')' : if (lx->paren_depth > 0) lx->paren_depth--; return make_tok(lx, TOK_RPAREN, start, 1);
        case ':' : return make_tok(lx, TOK_COLON, start, 1);
        case ',' : return make_tok(lx, TOK_COMMA, start, 1);
        case '.' : return make_tok(lx, TOK_DOT, start, 1);
        case '+' :
            if (peekc(lx) == '=') { lx->pos++; return make_tok(lx, TOK_PLUS_ASSIGN, start, 2); }
            return make_tok(lx, TOK_PLUS, start, 1);
        case '-' :
            if (peekc(lx) == '>') { lx->pos++; return make_tok(lx, TOK_ARROW, start, 2); }
            if (peekc(lx) == '=') { lx->pos++; return make_tok(lx, TOK_MINUS_ASSIGN, start, 2); }
            return make_tok(lx, TOK_MINUS, start, 1);
        case '*' : return make_tok(lx, TOK_STAR, start, 1);
        case '/' : return make_tok(lx, TOK_SLASH, start, 1);
        case '%' : return make_tok(lx, TOK_PERCENT, start, 1);
        case '=' :
            if (peekc(lx) == '=') { lx->pos++; return make_tok(lx, TOK_EQ, start, 2); }
            return make_tok(lx, TOK_ASSIGN, start, 1);
        case '!' :
            if (peekc(lx) == '=') { lx->pos++; return make_tok(lx, TOK_NEQ, start, 2); }
            return make_tok(lx, TOK_UNKNOWN, start, 1);
        case '<' :
            if (peekc(lx) == '=') { lx->pos++; return make_tok(lx, TOK_LE, start, 2); }
            return make_tok(lx, TOK_LT, start, 1);
        case '>' :
            if (peekc(lx) == '=') { lx->pos++; return make_tok(lx, TOK_GE, start, 2); }
            return make_tok(lx, TOK_GT, start, 1);
        default:
            return make_tok(lx, TOK_UNKNOWN, start, 1);
    }
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOK_EOF: return "EOF";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_INDENT: return "INDENT";
        case TOK_DEDENT: return "DEDENT";
        case TOK_IDENT: return "IDENT";
        case TOK_INT_LIT: return "INT_LIT";
        case TOK_FLOAT_LIT: return "FLOAT_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        case TOK_DEF: return "def";
        case TOK_IF: return "if";
        case TOK_ELIF: return "elif";
        case TOK_ELSE: return "else";
        case TOK_WHILE: return "while";
        case TOK_FOR: return "for";
        case TOK_IN: return "in";
        case TOK_RANGE: return "range";
        case TOK_RETURN: return "return";
        case TOK_IMPORT: return "import";
        case TOK_AND: return "and";
        case TOK_OR: return "or";
        case TOK_NOT: return "not";
        case TOK_TRUE: return "True";
        case TOK_FALSE: return "False";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_COLON: return ":";
        case TOK_COMMA: return ",";
        case TOK_DOT: return ".";
        case TOK_ARROW: return "->";
        case TOK_ASSIGN: return "=";
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_EQ: return "==";
        case TOK_NEQ: return "!=";
        case TOK_LT: return "<";
        case TOK_LE: return "<=";
        case TOK_GT: return ">";
        case TOK_GE: return ">=";
        case TOK_PLUS_ASSIGN: return "+=";
        case TOK_MINUS_ASSIGN: return "-=";
        default: return "UNKNOWN";
    }
}
