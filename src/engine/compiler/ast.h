#ifndef HERMES_AST_H
#define HERMES_AST_H

typedef enum {
    NODE_PROGRAM,
    NODE_IMPORT,
    NODE_FUNC_DEF,
    NODE_PARAM,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_COMPOUND_ASSIGN,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR_RANGE,
    NODE_PARALLEL_FOR,
    NODE_RETURN,
    NODE_EXPR_STMT,
    NODE_INSERT_C,

    NODE_BINOP,
    NODE_UNOP,
    NODE_LITERAL_INT,
    NODE_LITERAL_FLOAT,
    NODE_LITERAL_STRING,
    NODE_LITERAL_BOOL,
    NODE_IDENT,
    NODE_CALL,
    NODE_METHOD_CALL,
} NodeKind;

typedef struct Node Node;

#define PSCOPE_LOCAL 1
#define PSCOPE_GLOBAL 2

typedef struct {
    Node **items;
    int count;
    int capacity;
} NodeList;

/* one `elif`/`else if` branch: condition + block (condition is NULL for the
 * final `else`) */
typedef struct {
    Node *cond;      /* NULL means this is the else branch */
    NodeList body;
} IfBranch;

typedef struct {
    IfBranch *items;
    int count;
    int capacity;
} IfBranchList;

struct Node {
    NodeKind kind;
    int line;

    union {
        struct { NodeList items; } program;

        struct { char *module_name; } import_stmt;

        struct {
            char *name;
            NodeList params;      /* list of NODE_PARAM */
            char *return_type;    /* NULL => void */
            NodeList body;
        } func_def;

        struct { char *type_name; char *name; } param;

        /* `pscope`: 0 = normal declaration; PSCOPE_LOCAL / PSCOPE_GLOBAL =
         * `local` / `global` at the top of a parallel for's body.
         * `reduce`: for `global(op)`, the operator ("+", "*", "min",
         * "max"); NULL otherwise. */
        struct { char *type_name; char *name; Node *init; int pscope; char *reduce; } var_decl;

        struct { char *name; Node *value; } assign;

        struct { char *name; char op; Node *value; } compound_assign; /* op: '+' or '-' */

        struct { IfBranchList branches; } if_stmt;

        struct { Node *cond; NodeList body; } while_stmt;

        struct {
            char *var_name;
            Node *start, *end, *step; /* step may be NULL => 1 */
            NodeList body;
        } for_range;

        /* parallel for <var> in range(...) [threads <expr>]: -- same range
         * as for_range plus the thread count (NULL = automatic). */
        struct {
            char *var_name;
            Node *start, *end, *step; /* start NULL => 0, step NULL => 1 */
            Node *threads;
            NodeList body;
        } parallel_for;

        struct { Node *value; } return_stmt; /* value may be NULL */

        struct { Node *expr; } expr_stmt;

        /* insert_C: plain C copied verbatim into the generated .c, at the
         * place of the directive. `line` is the line of `insert_C`; the
         * code starts on the next one. */
        struct { char *code; } insert_c;

        struct { char op[3]; Node *left; Node *right; } binop;
        struct { char op[3]; Node *operand; } unop;

        struct { long value; } literal_int;
        struct { double value; } literal_float;
        struct { char *value; } literal_string;
        struct { int value; } literal_bool;
        struct { char *name; } ident;
        struct { char *name; NodeList args; } call;

        /* `receiver.method(args)` -- resolved by codegen at transpile time
         * to `device_<kind>_<method>(receiver, args...)` based on the
         * statically-known device kind of `receiver` (see
         * src/engine/compiler/devicekind.h). `receiver` is currently always
         * a NODE_IDENT in practice (no chaining support needed yet). */
        struct { Node *receiver; char *method_name; NodeList args; } method_call;
    } as;
};

Node *node_new(NodeKind kind, int line);
void node_list_push(NodeList *list, Node *node);
void if_branch_list_push(IfBranchList *list, IfBranch branch);

#endif
