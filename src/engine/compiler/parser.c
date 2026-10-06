#include "parser.h"
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Lexer lx;
    Token cur;
    Token next; /* one token of extra lookahead */
    int has_next;
    const char *filename;
} Parser;

static char *tok_strdup(Token t) {
    char *s = malloc(t.length + 1);
    memcpy(s, t.start, t.length);
    s[t.length] = '\0';
    return s;
}

static void advance(Parser *p) {
    if (p->has_next) {
        p->cur = p->next;
        p->has_next = 0;
    } else {
        p->cur = lexer_next(&p->lx);
    }
}

static Token peek_next(Parser *p) {
    if (!p->has_next) {
        p->next = lexer_next(&p->lx);
        p->has_next = 1;
    }
    return p->next;
}

static void fail(Parser *p, const char *msg) {
    fprintf(stderr, "%s:%d: error de sintaxis: %s (token '%.*s')\n",
            p->filename, p->cur.line, msg, p->cur.length, p->cur.start);
    exit(1);
}

static void expect(Parser *p, TokenKind kind, const char *what) {
    if (p->cur.kind != kind) {
        fprintf(stderr, "%s:%d: error de sintaxis: se esperaba %s pero se encontro '%.*s'\n",
                p->filename, p->cur.line, what, p->cur.length, p->cur.start);
        exit(1);
    }
    advance(p);
}

static int check(Parser *p, TokenKind kind) { return p->cur.kind == kind; }

static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) advance(p);
}

/* forward decls */
static Node *parse_expr(Parser *p);
static void parse_block(Parser *p, NodeList *out);
static Node *parse_stmt(Parser *p);

static Node *parse_primary(Parser *p) {
    int line = p->cur.line;
    if (check(p, TOK_INT_LIT)) {
        Node *n = node_new(NODE_LITERAL_INT, line);
        n->as.literal_int.value = p->cur.int_val;
        advance(p);
        return n;
    }
    if (check(p, TOK_FLOAT_LIT)) {
        Node *n = node_new(NODE_LITERAL_FLOAT, line);
        n->as.literal_float.value = p->cur.float_val;
        advance(p);
        return n;
    }
    if (check(p, TOK_STRING_LIT)) {
        Node *n = node_new(NODE_LITERAL_STRING, line);
        n->as.literal_string.value = tok_strdup(p->cur);
        advance(p);
        return n;
    }
    if (check(p, TOK_TRUE) || check(p, TOK_FALSE)) {
        Node *n = node_new(NODE_LITERAL_BOOL, line);
        n->as.literal_bool.value = check(p, TOK_TRUE) ? 1 : 0;
        advance(p);
        return n;
    }
    if (check(p, TOK_IDENT)) {
        char *name = tok_strdup(p->cur);
        advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            Node *n = node_new(NODE_CALL, line);
            n->as.call.name = name;
            if (!check(p, TOK_RPAREN)) {
                node_list_push(&n->as.call.args, parse_expr(p));
                while (check(p, TOK_COMMA)) { advance(p); node_list_push(&n->as.call.args, parse_expr(p)); }
            }
            expect(p, TOK_RPAREN, "')'");
            return n;
        }
        Node *n = node_new(NODE_IDENT, line);
        n->as.ident.name = name;
        return n;
    }
    if (check(p, TOK_LPAREN)) {
        advance(p);
        Node *n = parse_expr(p);
        expect(p, TOK_RPAREN, "')'");
        return n;
    }
    fail(p, "expresion invalida");
    return NULL;
}

/* receptor.metodo(args)*, encadenable (aunque hoy solo se usa una vez: el
 * receptor en la practica es siempre un NODE_IDENT, ver devicekind.h).
 * Compartido entre parse_postfix (dentro de una expresion) y
 * parse_assign_or_expr_stmt (cuando la llamada a metodo es la instruccion
 * completa, p. ej. "led.write(True)" suelto en una linea). */
static Node *apply_method_calls(Parser *p, Node *n) {
    while (check(p, TOK_DOT)) {
        int line = p->cur.line;
        advance(p);
        char *method_name = tok_strdup(p->cur);
        expect(p, TOK_IDENT, "nombre de metodo");
        expect(p, TOK_LPAREN, "'('");
        Node *call = node_new(NODE_METHOD_CALL, line);
        call->as.method_call.receiver = n;
        call->as.method_call.method_name = method_name;
        if (!check(p, TOK_RPAREN)) {
            node_list_push(&call->as.method_call.args, parse_expr(p));
            while (check(p, TOK_COMMA)) { advance(p); node_list_push(&call->as.method_call.args, parse_expr(p)); }
        }
        expect(p, TOK_RPAREN, "')'");
        n = call;
    }
    return n;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    return apply_method_calls(p, n);
}

static Node *parse_unary(Parser *p) {
    int line = p->cur.line;
    if (check(p, TOK_MINUS)) {
        advance(p);
        Node *n = node_new(NODE_UNOP, line);
        strcpy(n->as.unop.op, "-");
        n->as.unop.operand = parse_unary(p);
        return n;
    }
    if (check(p, TOK_NOT)) {
        advance(p);
        Node *n = node_new(NODE_UNOP, line);
        strcpy(n->as.unop.op, "!");
        n->as.unop.operand = parse_unary(p);
        return n;
    }
    return parse_postfix(p);
}

static Node *parse_mul(Parser *p) {
    Node *left = parse_unary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        int line = p->cur.line;
        const char *op = check(p, TOK_STAR) ? "*" : check(p, TOK_SLASH) ? "/" : "%";
        advance(p);
        Node *n = node_new(NODE_BINOP, line);
        strcpy(n->as.binop.op, op);
        n->as.binop.left = left;
        n->as.binop.right = parse_unary(p);
        left = n;
    }
    return left;
}

static Node *parse_add(Parser *p) {
    Node *left = parse_mul(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        int line = p->cur.line;
        const char *op = check(p, TOK_PLUS) ? "+" : "-";
        advance(p);
        Node *n = node_new(NODE_BINOP, line);
        strcpy(n->as.binop.op, op);
        n->as.binop.left = left;
        n->as.binop.right = parse_mul(p);
        left = n;
    }
    return left;
}

static Node *parse_comparison(Parser *p) {
    Node *left = parse_add(p);
    while (check(p, TOK_EQ) || check(p, TOK_NEQ) || check(p, TOK_LT) ||
           check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE)) {
        int line = p->cur.line;
        const char *op = check(p, TOK_EQ) ? "==" : check(p, TOK_NEQ) ? "!=" :
                         check(p, TOK_LT) ? "<" : check(p, TOK_LE) ? "<=" :
                         check(p, TOK_GT) ? ">" : ">=";
        advance(p);
        Node *n = node_new(NODE_BINOP, line);
        strcpy(n->as.binop.op, op);
        n->as.binop.left = left;
        n->as.binop.right = parse_add(p);
        left = n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_comparison(p);
    while (check(p, TOK_AND)) {
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(NODE_BINOP, line);
        strcpy(n->as.binop.op, "&&");
        n->as.binop.left = left;
        n->as.binop.right = parse_comparison(p);
        left = n;
    }
    return left;
}

static Node *parse_or(Parser *p) {
    Node *left = parse_and(p);
    while (check(p, TOK_OR)) {
        int line = p->cur.line;
        advance(p);
        Node *n = node_new(NODE_BINOP, line);
        strcpy(n->as.binop.op, "||");
        n->as.binop.left = left;
        n->as.binop.right = parse_and(p);
        left = n;
    }
    return left;
}

static Node *parse_expr(Parser *p) { return parse_or(p); }

/* a "type" is just an identifier (int, float, char, bool, uint8_t, void, ...)
 * passed straight through to the generated C. */
static int looks_like_type_then_name(Parser *p) {
    return check(p, TOK_IDENT) && peek_next(p).kind == TOK_IDENT;
}

static Node *parse_var_decl(Parser *p) {
    int line = p->cur.line;
    char *type_name = tok_strdup(p->cur);
    advance(p);
    char *name = tok_strdup(p->cur);
    advance(p);
    Node *n = node_new(NODE_VAR_DECL, line);
    n->as.var_decl.type_name = type_name;
    n->as.var_decl.name = name;
    n->as.var_decl.init = NULL;
    if (check(p, TOK_ASSIGN)) {
        advance(p);
        n->as.var_decl.init = parse_expr(p);
    }
    expect(p, TOK_NEWLINE, "fin de linea");
    return n;
}

static Node *parse_assign_or_expr_stmt(Parser *p) {
    int line = p->cur.line;
    char *name = tok_strdup(p->cur);
    advance(p);

    if (check(p, TOK_ASSIGN)) {
        advance(p);
        Node *n = node_new(NODE_ASSIGN, line);
        n->as.assign.name = name;
        n->as.assign.value = parse_expr(p);
        expect(p, TOK_NEWLINE, "fin de linea");
        return n;
    }
    if (check(p, TOK_PLUS_ASSIGN) || check(p, TOK_MINUS_ASSIGN)) {
        char op = check(p, TOK_PLUS_ASSIGN) ? '+' : '-';
        advance(p);
        Node *n = node_new(NODE_COMPOUND_ASSIGN, line);
        n->as.compound_assign.name = name;
        n->as.compound_assign.op = op;
        n->as.compound_assign.value = parse_expr(p);
        expect(p, TOK_NEWLINE, "fin de linea");
        return n;
    }
    if (check(p, TOK_LPAREN)) {
        advance(p);
        Node *call = node_new(NODE_CALL, line);
        call->as.call.name = name;
        if (!check(p, TOK_RPAREN)) {
            node_list_push(&call->as.call.args, parse_expr(p));
            while (check(p, TOK_COMMA)) { advance(p); node_list_push(&call->as.call.args, parse_expr(p)); }
        }
        expect(p, TOK_RPAREN, "')'");
        Node *stmt = node_new(NODE_EXPR_STMT, line);
        stmt->as.expr_stmt.expr = call;
        expect(p, TOK_NEWLINE, "fin de linea");
        return stmt;
    }
    if (check(p, TOK_DOT)) {
        Node *ident = node_new(NODE_IDENT, line);
        ident->as.ident.name = name;
        Node *call = apply_method_calls(p, ident);
        Node *stmt = node_new(NODE_EXPR_STMT, line);
        stmt->as.expr_stmt.expr = call;
        expect(p, TOK_NEWLINE, "fin de linea");
        return stmt;
    }
    fail(p, "se esperaba una asignacion o llamada a funcion/metodo");
    return NULL;
}

static Node *parse_if(Parser *p) {
    int line = p->cur.line;
    Node *n = node_new(NODE_IF, line);
    advance(p); /* 'if' */
    IfBranch branch;
    branch.cond = parse_expr(p);
    branch.body = (NodeList){0};
    expect(p, TOK_COLON, "':'");
    parse_block(p, &branch.body);
    if_branch_list_push(&n->as.if_stmt.branches, branch);

    while (check(p, TOK_ELIF)) {
        advance(p);
        IfBranch b2;
        b2.cond = parse_expr(p);
        b2.body = (NodeList){0};
        expect(p, TOK_COLON, "':'");
        parse_block(p, &b2.body);
        if_branch_list_push(&n->as.if_stmt.branches, b2);
    }
    if (check(p, TOK_ELSE)) {
        advance(p);
        IfBranch b3;
        b3.cond = NULL;
        b3.body = (NodeList){0};
        expect(p, TOK_COLON, "':'");
        parse_block(p, &b3.body);
        if_branch_list_push(&n->as.if_stmt.branches, b3);
    }
    return n;
}

static Node *parse_while(Parser *p) {
    int line = p->cur.line;
    advance(p); /* 'while' */
    Node *n = node_new(NODE_WHILE, line);
    n->as.while_stmt.cond = parse_expr(p);
    expect(p, TOK_COLON, "':'");
    parse_block(p, &n->as.while_stmt.body);
    return n;
}

static Node *parse_for(Parser *p) {
    int line = p->cur.line;
    advance(p); /* 'for' */
    char *var_name = tok_strdup(p->cur);
    expect(p, TOK_IDENT, "identificador");
    expect(p, TOK_IN, "'in'");
    expect(p, TOK_RANGE, "'range'");
    expect(p, TOK_LPAREN, "'('");

    Node *a1 = parse_expr(p);
    Node *start = NULL, *end = NULL, *step = NULL;
    if (check(p, TOK_COMMA)) {
        advance(p);
        Node *a2 = parse_expr(p);
        start = a1; end = a2;
        if (check(p, TOK_COMMA)) {
            advance(p);
            step = parse_expr(p);
        }
    } else {
        start = NULL; end = a1;
    }
    expect(p, TOK_RPAREN, "')'");
    expect(p, TOK_COLON, "':'");

    Node *n = node_new(NODE_FOR_RANGE, line);
    n->as.for_range.var_name = var_name;
    n->as.for_range.start = start;
    n->as.for_range.end = end;
    n->as.for_range.step = step;
    parse_block(p, &n->as.for_range.body);
    return n;
}

static Node *parse_return(Parser *p) {
    int line = p->cur.line;
    advance(p); /* 'return' */
    Node *n = node_new(NODE_RETURN, line);
    if (!check(p, TOK_NEWLINE)) {
        n->as.return_stmt.value = parse_expr(p);
    }
    expect(p, TOK_NEWLINE, "fin de linea");
    return n;
}

static Node *parse_stmt(Parser *p) {
    if (check(p, TOK_IF)) return parse_if(p);
    if (check(p, TOK_WHILE)) return parse_while(p);
    if (check(p, TOK_FOR)) return parse_for(p);
    if (check(p, TOK_RETURN)) return parse_return(p);
    if (looks_like_type_then_name(p)) return parse_var_decl(p);
    if (check(p, TOK_IDENT)) return parse_assign_or_expr_stmt(p);
    fail(p, "instruccion no reconocida");
    return NULL;
}

static void parse_block(Parser *p, NodeList *out) {
    expect(p, TOK_NEWLINE, "fin de linea antes de un bloque indentado");
    skip_newlines(p);
    expect(p, TOK_INDENT, "bloque indentado");
    skip_newlines(p);
    while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
        node_list_push(out, parse_stmt(p));
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT, "fin de bloque indentado");
}

static Node *parse_func_def(Parser *p) {
    int line = p->cur.line;
    advance(p); /* 'def' */
    char *name = tok_strdup(p->cur);
    expect(p, TOK_IDENT, "nombre de funcion");
    expect(p, TOK_LPAREN, "'('");

    Node *n = node_new(NODE_FUNC_DEF, line);
    n->as.func_def.name = name;
    n->as.func_def.return_type = NULL;

    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            int pline = p->cur.line;
            char *ptype = tok_strdup(p->cur);
            expect(p, TOK_IDENT, "tipo de parametro");
            char *pname = tok_strdup(p->cur);
            expect(p, TOK_IDENT, "nombre de parametro");
            Node *param = node_new(NODE_PARAM, pline);
            param->as.param.type_name = ptype;
            param->as.param.name = pname;
            node_list_push(&n->as.func_def.params, param);
            if (check(p, TOK_COMMA)) { advance(p); continue; }
            break;
        }
    }
    expect(p, TOK_RPAREN, "')'");
    if (check(p, TOK_ARROW)) {
        advance(p);
        n->as.func_def.return_type = tok_strdup(p->cur);
        expect(p, TOK_IDENT, "tipo de retorno");
    }
    expect(p, TOK_COLON, "':'");
    parse_block(p, &n->as.func_def.body);
    return n;
}

static Node *parse_import(Parser *p) {
    int line = p->cur.line;
    advance(p); /* 'import' */
    Node *n = node_new(NODE_IMPORT, line);
    n->as.import_stmt.module_name = tok_strdup(p->cur);
    expect(p, TOK_IDENT, "nombre de modulo");
    expect(p, TOK_NEWLINE, "fin de linea");
    return n;
}

Node *parse_program(const char *src, const char *filename) {
    Parser p;
    lexer_init(&p.lx, src);
    p.has_next = 0;
    p.filename = filename;
    advance(&p);

    Node *program = node_new(NODE_PROGRAM, 1);
    skip_newlines(&p);
    while (!check(&p, TOK_EOF)) {
        if (check(&p, TOK_IMPORT)) node_list_push(&program->as.program.items, parse_import(&p));
        else if (check(&p, TOK_DEF)) node_list_push(&program->as.program.items, parse_func_def(&p));
        else if (looks_like_type_then_name(&p)) node_list_push(&program->as.program.items, parse_var_decl(&p));
        else fail(&p, "en el nivel superior solo se permiten 'import', 'def' y declaraciones de variables globales");
        skip_newlines(&p);
    }
    return program;
}
