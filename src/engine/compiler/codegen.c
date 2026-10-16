#include "codegen.h"
#include "arch.h"
#include "board.h"
#include "threadprofile.h"
#include "devicekind.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    char *data;
    int len;
    int capacity;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->capacity = 4096;
    sb->data = malloc(sb->capacity);
    sb->data[0] = '\0';
    sb->len = 0;
}

static void sb_append(StrBuf *sb, const char *text) {
    int add_len = (int)strlen(text);
    while (sb->len + add_len + 1 > sb->capacity) {
        sb->capacity *= 2;
        sb->data = realloc(sb->data, sb->capacity);
    }
    memcpy(sb->data + sb->len, text, add_len + 1);
    sb->len += add_len;
}

static void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    sb_append(sb, tmp);
}

static void indent(StrBuf *sb, int depth) {
    for (int i = 0; i < depth; i++) sb_append(sb, "    ");
}

/* --- .hepy line mapping ----------------------------------------------------
 * Before every statement, function and global, a `#line N "file.hepy"`:
 * the C compiler then reports any error or warning at the .hepy line the
 * code came from (and shows that .hepy line), instead of at the generated
 * .c. Generated-only code (prototypes, a parallel for's chunk function)
 * gets the line of the .hepy construct it comes from. */
static const char *g_source_path = NULL; /* escaped for a C string literal */

static void emit_line(StrBuf *sb, int line) {
    if (!g_source_path || line <= 0) return;
    sb_appendf(sb, "#line %d \"%s\"\n", line, g_source_path);
}

/* hepyc's own errors, in the same `file:line: error: ...` form as gcc */
static const char *g_source_display = "<entrada>";

static void emit_expr(StrBuf *sb, Node *n);
static void emit_block(StrBuf *sb, NodeList *block, int depth);
static void emit_stmt(StrBuf *sb, Node *n, int depth);
static void emit_parallel_for(StrBuf *sb, Node *n, int depth);

/* --- device-kind tracking ------------------------------------------------
 *
 * `device x = ctor(...)` is special-cased: if `ctor` is a recognized device
 * constructor (devicekind.h), the HERMESPY type `device` is replaced in the
 * generated C by the concrete handle type for that kind, and (name -> kind)
 * is remembered so that a later `x.method(args)` can be resolved to
 * `device_<kind>_method(x, args...)` -- purely at transpile time, no
 * runtime dispatch. Two tables: one for globals (persists for the whole
 * program) and one for the function currently being emitted (reset at the
 * start of each function body). A `device`-typed value whose kind can't be
 * determined this way (e.g. a function parameter) falls back to the
 * generic handle `hermes_handle_t` and simply can't be used with `.method()`
 * -- see docs/raw/hermes_devices.en.md. */
typedef struct { const char *name; const char *kind; } DeviceVar;
#define MAX_DEVICE_VARS 256
static DeviceVar g_global_devices[MAX_DEVICE_VARS];
static int g_global_device_count = 0;
static DeviceVar g_local_devices[MAX_DEVICE_VARS];
static int g_local_device_count = 0;
static int g_emitting_function_body = 0;

static void device_var_register(const char *name, const char *kind) {
    DeviceVar *table = g_emitting_function_body ? g_local_devices : g_global_devices;
    int *count = g_emitting_function_body ? &g_local_device_count : &g_global_device_count;
    if (*count < MAX_DEVICE_VARS) {
        table[*count].name = name;
        table[*count].kind = kind;
        (*count)++;
    }
}

static const char *device_var_lookup(const char *name) {
    for (int i = g_local_device_count - 1; i >= 0; i--) {
        if (strcmp(g_local_devices[i].name, name) == 0) return g_local_devices[i].kind;
    }
    for (int i = g_global_device_count - 1; i >= 0; i--) {
        if (strcmp(g_global_devices[i].name, name) == 0) return g_global_devices[i].kind;
    }
    return NULL;
}

/* --- local scope tracking ------------------------------------------------
 * Names (and C types) of the parameters and local variables visible at
 * the point being emitted in the current function. Used by `parallel for`:
 * its body becomes a separate C function, so the enclosing function's
 * locals it reads have to be copied in (see emit_parallel_for). Program
 * globals aren't tracked: they're file-scope in C, visible everywhere. */
typedef struct { const char *name; const char *c_type; } ScopeVar;
#define MAX_SCOPE_VARS 1024
static ScopeVar g_scope[MAX_SCOPE_VARS];
static int g_scope_count = 0;

static void scope_push(const char *name, const char *c_type) {
    if (!g_emitting_function_body) return;
    if (g_scope_count >= MAX_SCOPE_VARS) {
        fprintf(stderr, "codegen: too many local variables (at most %d)\n", MAX_SCOPE_VARS);
        exit(1);
    }
    g_scope[g_scope_count].name = name;
    g_scope[g_scope_count].c_type = c_type;
    g_scope_count++;
}

static const char *scope_lookup(const char *name) {
    for (int i = g_scope_count - 1; i >= 0; i--) {
        if (strcmp(g_scope[i].name, name) == 0) return g_scope[i].c_type;
    }
    return NULL;
}

/* --- identifier renaming --------------------------------------------------
 * Inside a parallel for's chunk function, a shared `global` is reached
 * through the context struct: `total` is emitted as `(*hepy_ctx->total)`. */
typedef struct { const char *name; char *expr; } Rename;
#define MAX_RENAMES 256
static Rename g_renames[MAX_RENAMES];
static int g_rename_count = 0;

static const char *ident_text(const char *name) {
    for (int i = g_rename_count - 1; i >= 0; i--) {
        if (strcmp(g_renames[i].name, name) == 0) return g_renames[i].expr;
    }
    return name;
}

/* Generated chunk functions of the function being emitted: they go right
 * before it in the .c (user function prototypes are already above). */
static StrBuf g_pre;
static int g_pre_ready = 0;
static int g_pfor_count = 0;
static int g_parallel_imported = 0;
static int g_in_parallel_body = 0;

/* Names of the functions the program calls (codegen_calls). */
#define CODEGEN_MAX_CALLED 256
static const char *g_called[CODEGEN_MAX_CALLED];
static int g_called_count;

static void note_call(const char *name) {
    for (int i = 0; i < g_called_count; i++) if (strcmp(g_called[i], name) == 0) return;
    if (g_called_count < CODEGEN_MAX_CALLED) g_called[g_called_count++] = name;
}

int codegen_calls(const char *name) {
    for (int i = 0; i < g_called_count; i++) if (strcmp(g_called[i], name) == 0) return 1;
    return 0;
}

/* Methods the program calls (any kind), and whether it names a host
 * (DNS) in one: codegen_calls_method, codegen_names_hosts. */
static const char *g_methods[CODEGEN_MAX_CALLED];
static int g_method_count, g_names_hosts;

static void note_method(const char *name) {
    for (int i = 0; i < g_method_count; i++) if (strcmp(g_methods[i], name) == 0) return;
    if (g_method_count < CODEGEN_MAX_CALLED) g_methods[g_method_count++] = name;
}

int codegen_calls_method(const char *name) {
    for (int i = 0; i < g_method_count; i++) if (strcmp(g_methods[i], name) == 0) return 1;
    return 0;
}

int codegen_names_hosts(void) { return g_names_hosts; }

/* the address argument of x.tcp(...) / x.send_to(...): a name unless it
 * is a literal "a.b.c.d" (a variable may hold a name) */
static void note_address_arg(Node *a) {
    if (a->kind != NODE_LITERAL_STRING) { g_names_hosts = 1; return; }
    for (const char *s = a->as.literal_string.value; *s; s++)
        if (!((*s >= '0' && *s <= '9') || *s == '.')) { g_names_hosts = 1; return; }
}

static void emit_call_args(StrBuf *sb, NodeList *args) {
    for (int i = 0; i < args->count; i++) {
        if (i > 0) sb_append(sb, ", ");
        emit_expr(sb, args->items[i]);
    }
}

static void emit_expr(StrBuf *sb, Node *n) {
    switch (n->kind) {
        case NODE_LITERAL_INT:
            sb_appendf(sb, "%ld", n->as.literal_int.value);
            break;
        case NODE_LITERAL_FLOAT:
            sb_appendf(sb, "%g", n->as.literal_float.value);
            break;
        case NODE_LITERAL_STRING:
            sb_appendf(sb, "\"%s\"", n->as.literal_string.value);
            break;
        case NODE_LITERAL_BOOL:
            /* Plain `true`/`false` would have type `int` (they're just
             * `#define`d to 1/0 in <stdbool.h>), which makes a bare literal
             * invisible to `_Generic`'s `bool` case (see `print` in
             * hermes_stdlib.h) -- it always matches `int` instead. The cast
             * forces the expression's type to genuinely be `bool`. */
            sb_append(sb, n->as.literal_bool.value ? "(bool)true" : "(bool)false");
            break;
        case NODE_IDENT:
            sb_append(sb, ident_text(n->as.ident.name));
            break;
        case NODE_CALL:
            if (strcmp(n->as.call.name, "thread_destroy") == 0 && n->as.call.args.count == 1 &&
                n->as.call.args.items[0]->kind == NODE_IDENT) {
                /* same as t.destroy(): the variable is left 0 (no handle) */
                const char *v = ident_text(n->as.call.args.items[0]->as.ident.name);
                sb_appendf(sb, "(thread_destroy(%s), %s = 0)", v, v);
                break;
            }
            note_call(n->as.call.name);
            sb_append(sb, n->as.call.name);
            sb_append(sb, "(");
            emit_call_args(sb, &n->as.call.args);
            sb_append(sb, ")");
            break;
        case NODE_BINOP:
            sb_append(sb, "(");
            emit_expr(sb, n->as.binop.left);
            sb_appendf(sb, " %s ", n->as.binop.op);
            emit_expr(sb, n->as.binop.right);
            sb_append(sb, ")");
            break;
        case NODE_UNOP:
            sb_appendf(sb, "(%s", n->as.unop.op);
            emit_expr(sb, n->as.unop.operand);
            sb_append(sb, ")");
            break;
        case NODE_METHOD_CALL: {
            Node *recv = n->as.method_call.receiver;
            if (recv->kind != NODE_IDENT) {
                fprintf(stderr,
                    "%s:%d: error: a method can only be called on a variable "
                    "directly (e.g. 'x.method(...)'), not on an expression\n", g_source_display, n->line);
                exit(1);
            }
            const char *kind = device_var_lookup(recv->as.ident.name);
            if (!kind) {
                fprintf(stderr,
                    "%s:%d: error: can't tell the device kind of '%s' to call '.%s(...)': "
                    "methods only work on a 'device' variable declared with a known device "
                    "constructor (see src/engine/compiler/devicekind.c); generic 'device' "
                    "parameters don't support method calls\n",
                    g_source_display, n->line, recv->as.ident.name, n->as.method_call.method_name);
                exit(1);
            }
            if ((strcmp(n->as.method_call.method_name, "destroy") == 0 ||
                 strcmp(n->as.method_call.method_name, "safe_destroy") == 0) &&
                n->as.method_call.args.count == 0) {
                /* x.destroy() also leaves the variable 0 (no handle: see
                 * "handles" in hermes_device.h), for every kind
                 * (hermes_thread_destroy, hermes_device_destroy):
                 * `(device_<kind>_destroy(x), x = 0)`; the same for the
                 * i2c bus's safe_destroy() */
                const char *v = ident_text(recv->as.ident.name);
                sb_appendf(sb, "(device_%s_%s(%s), %s = 0)", kind, n->as.method_call.method_name, v, v);
                break;
            }
            note_method(n->as.method_call.method_name);
            if ((strcmp(n->as.method_call.method_name, "tcp") == 0 || strcmp(n->as.method_call.method_name, "send_to") == 0) &&
                n->as.method_call.args.count > 0)
                note_address_arg(n->as.method_call.args.items[0]);
            sb_appendf(sb, "device_%s_%s(%s", kind, n->as.method_call.method_name, ident_text(recv->as.ident.name));
            if (n->as.method_call.args.count > 0) {
                sb_append(sb, ", ");
                emit_call_args(sb, &n->as.method_call.args);
            }
            sb_append(sb, ")");
            break;
        }
        default:
            fprintf(stderr, "codegen: unexpected expression node (kind=%d)\n", n->kind);
            exit(1);
    }
}

static void emit_if(StrBuf *sb, Node *n, int depth) {
    IfBranchList *branches = &n->as.if_stmt.branches;
    for (int i = 0; i < branches->count; i++) {
        IfBranch *b = &branches->items[i];
        if (i == 0) {
            indent(sb, depth);
            sb_append(sb, "if (");
            emit_expr(sb, b->cond);
            sb_append(sb, ") {\n");
        } else if (b->cond != NULL) {
            emit_line(sb, b->cond->line);
            indent(sb, depth);
            sb_append(sb, "} else if (");
            emit_expr(sb, b->cond);
            sb_append(sb, ") {\n");
        } else {
            indent(sb, depth);
            sb_append(sb, "} else {\n");
        }
        emit_block(sb, &b->body, depth + 1);
    }
    indent(sb, depth);
    sb_append(sb, "}\n");
}

static void emit_stmt(StrBuf *sb, Node *n, int depth) {
    if (n->kind == NODE_INSERT_C) {
        /* plain C, verbatim: not indented, not looked at. Its first line
         * is the one after `insert_C:`, so the C compiler's messages
         * about it point at the right .hepy line. */
        emit_line(sb, n->line + 1);
        sb_append(sb, n->as.insert_c.code);
        if (sb->len > 0 && sb->data[sb->len - 1] != '\n') sb_append(sb, "\n");
        return;
    }
    emit_line(sb, n->line);
    switch (n->kind) {
        case NODE_VAR_DECL: {
            const char *type_name = n->as.var_decl.type_name;
            if (strcmp(type_name, "device") == 0) {
                /* Infer the concrete kind from the initializer, if it's a
                 * call to a recognized device constructor; see the
                 * device-kind tracking comment above. Falls back to the
                 * generic base type (no kind tracked -> no .method()
                 * calls possible on this variable later). */
                const HermesDeviceKind *dk = NULL;
                HermesDeviceKind from_method;
                Node *init = n->as.var_decl.init;
                if (init && init->kind == NODE_CALL) {
                    dk = hermes_devicekind_lookup_constructor(init->as.call.name);
                } else if (init && init->kind == NODE_METHOD_CALL &&
                           init->as.method_call.receiver->kind == NODE_IDENT) {
                    /* a method that creates a device (bus.at(0x68)) */
                    const char *rk = device_var_lookup(init->as.method_call.receiver->as.ident.name);
                    if (rk) dk = hermes_devicekind_lookup_method(rk, init->as.method_call.method_name, &from_method);
                }
                type_name = dk ? dk->c_type : "hermes_handle_t";
                if (dk) device_var_register(n->as.var_decl.name, dk->kind);
            }
            if (n->as.var_decl.pscope && !g_in_parallel_body) {
                fprintf(stderr, "%s:%d: error: '%s' can only be used at the top level of a "
                        "'parallel for' body\n", g_source_display, n->line,
                        n->as.var_decl.pscope == PSCOPE_LOCAL ? "local" : "global");
                exit(1);
            }
            scope_push(n->as.var_decl.name, type_name);
            indent(sb, depth);
            sb_appendf(sb, "%s %s", type_name, n->as.var_decl.name);
            if (n->as.var_decl.init) {
                sb_append(sb, " = ");
                emit_expr(sb, n->as.var_decl.init);
            }
            sb_append(sb, ";\n");
            break;
        }
        case NODE_ASSIGN:
            indent(sb, depth);
            sb_appendf(sb, "%s = ", ident_text(n->as.assign.name));
            emit_expr(sb, n->as.assign.value);
            sb_append(sb, ";\n");
            break;
        case NODE_COMPOUND_ASSIGN:
            indent(sb, depth);
            sb_appendf(sb, "%s %c= ", ident_text(n->as.compound_assign.name), n->as.compound_assign.op);
            emit_expr(sb, n->as.compound_assign.value);
            sb_append(sb, ";\n");
            break;
        case NODE_IF:
            emit_if(sb, n, depth);
            break;
        case NODE_WHILE:
            indent(sb, depth);
            sb_append(sb, "while (");
            emit_expr(sb, n->as.while_stmt.cond);
            sb_append(sb, ") {\n");
            emit_block(sb, &n->as.while_stmt.body, depth + 1);
            indent(sb, depth);
            sb_append(sb, "}\n");
            break;
        case NODE_FOR_RANGE: {
            const char *var = n->as.for_range.var_name;
            indent(sb, depth);
            sb_appendf(sb, "for (int %s = ", var);
            if (n->as.for_range.start) emit_expr(sb, n->as.for_range.start);
            else sb_append(sb, "0");
            sb_appendf(sb, "; %s < ", var);
            emit_expr(sb, n->as.for_range.end);
            sb_appendf(sb, "; %s += ", var);
            if (n->as.for_range.step) emit_expr(sb, n->as.for_range.step);
            else sb_append(sb, "1");
            sb_append(sb, ") {\n");
            int mark = g_scope_count;
            scope_push(var, "int");
            emit_block(sb, &n->as.for_range.body, depth + 1);
            g_scope_count = mark;
            indent(sb, depth);
            sb_append(sb, "}\n");
            break;
        }
        case NODE_PARALLEL_FOR:
            emit_parallel_for(sb, n, depth);
            break;
        case NODE_RETURN:
            indent(sb, depth);
            sb_append(sb, "return");
            if (n->as.return_stmt.value) {
                sb_append(sb, " ");
                emit_expr(sb, n->as.return_stmt.value);
            }
            sb_append(sb, ";\n");
            break;
        case NODE_EXPR_STMT:
            indent(sb, depth);
            emit_expr(sb, n->as.expr_stmt.expr);
            sb_append(sb, ";\n");
            break;
        default:
            fprintf(stderr, "codegen: unexpected statement (kind=%d)\n", n->kind);
            exit(1);
    }
}

static void emit_block(StrBuf *sb, NodeList *block, int depth) {
    int mark = g_scope_count;
    for (int i = 0; i < block->count; i++) {
        emit_stmt(sb, block->items[i], depth);
    }
    g_scope_count = mark;
}

/* --- parallel for ---------------------------------------------------------
 *
 *   parallel for i in range(a, b, s) threads n:
 *       global(+) long total = 0     # shared; each thread sums its own copy,
 *                                    # combined at the end
 *       global long seen = 0         # shared, accessed directly (the user
 *                                    # protects it, e.g. with a semaphore)
 *       local long mine = 0          # one per thread
 *       ...body...
 *
 * becomes a chunk function emitted before the enclosing function, plus a
 * call to hermes_parallel_run (hermes_parallel.h):
 *
 *   struct hepy_pfor_1 { char hepy_unused; long *total; long *seen; long n; };
 *   static void hepy_pfor_1(long hepy_lo, long hepy_hi, long hepy_step, void *hepy_vctx) {
 *       struct hepy_pfor_1 *hepy_ctx = hepy_vctx;
 *       long const n = hepy_ctx->n;          // enclosing local: read-only copy
 *       long total = 0;                       // reduction: private copy
 *       long mine = 0;                        // local
 *       for (long i = hepy_lo; i < hepy_hi; i += hepy_step) {
 *           ... `seen` emitted as (*hepy_ctx->seen) ...
 *       }
 *       hermes_parallel_lock(); *hepy_ctx->total += total; hermes_parallel_unlock();
 *   }
 *
 * The globals themselves are declared in the enclosing function, before
 * the loop (initialized once), so they stay usable after it. */

typedef struct { const char **items; int count; int capacity; } NameSet;

static int nameset_has(NameSet *set, const char *name) {
    for (int i = 0; i < set->count; i++) if (strcmp(set->items[i], name) == 0) return 1;
    return 0;
}

static void nameset_add(NameSet *set, const char *name) {
    if (nameset_has(set, name)) return;
    if (set->count == set->capacity) {
        set->capacity = set->capacity ? set->capacity * 2 : 16;
        set->items = realloc(set->items, sizeof(const char *) * set->capacity);
    }
    set->items[set->count++] = name;
}

static void pfor_fail(Node *n, const char *msg, const char *name) {
    fprintf(stderr, "%s:%d: error: parallel for: ", g_source_display, n->line);
    fprintf(stderr, msg, name);
    fprintf(stderr, "\n");
    exit(1);
}

/* Every identifier read in `n` (`used`), every name declared in it
 * (`declared`) and every name assigned in it (`assigned`). Also rejects
 * what can't go inside a parallel for's body. */
static void pfor_walk(Node *n, NameSet *used, NameSet *declared, NameSet *assigned);

static void pfor_walk_list(NodeList *list, NameSet *used, NameSet *declared, NameSet *assigned) {
    for (int i = 0; i < list->count; i++) pfor_walk(list->items[i], used, declared, assigned);
}

static void pfor_walk(Node *n, NameSet *used, NameSet *declared, NameSet *assigned) {
    if (!n) return;
    switch (n->kind) {
        case NODE_IDENT: nameset_add(used, n->as.ident.name); break;
        case NODE_CALL:
            if (strcmp(n->as.call.name, "thread_destroy") == 0 && n->as.call.args.count == 1 &&
                n->as.call.args.items[0]->kind == NODE_IDENT) {
                nameset_add(assigned, n->as.call.args.items[0]->as.ident.name); /* left NULL */
            }
            pfor_walk_list(&n->as.call.args, used, declared, assigned);
            break;
        case NODE_METHOD_CALL:
            if (strcmp(n->as.method_call.method_name, "destroy") == 0 &&
                n->as.method_call.receiver->kind == NODE_IDENT) {
                nameset_add(assigned, n->as.method_call.receiver->as.ident.name); /* left NULL */
            }
            pfor_walk(n->as.method_call.receiver, used, declared, assigned);
            pfor_walk_list(&n->as.method_call.args, used, declared, assigned);
            break;
        case NODE_BINOP:
            pfor_walk(n->as.binop.left, used, declared, assigned);
            pfor_walk(n->as.binop.right, used, declared, assigned);
            break;
        case NODE_UNOP: pfor_walk(n->as.unop.operand, used, declared, assigned); break;
        case NODE_VAR_DECL:
            nameset_add(declared, n->as.var_decl.name);
            pfor_walk(n->as.var_decl.init, used, declared, assigned);
            break;
        case NODE_ASSIGN:
            nameset_add(used, n->as.assign.name);
            nameset_add(assigned, n->as.assign.name);
            pfor_walk(n->as.assign.value, used, declared, assigned);
            break;
        case NODE_COMPOUND_ASSIGN:
            nameset_add(used, n->as.compound_assign.name);
            nameset_add(assigned, n->as.compound_assign.name);
            pfor_walk(n->as.compound_assign.value, used, declared, assigned);
            break;
        case NODE_IF:
            for (int i = 0; i < n->as.if_stmt.branches.count; i++) {
                pfor_walk(n->as.if_stmt.branches.items[i].cond, used, declared, assigned);
                pfor_walk_list(&n->as.if_stmt.branches.items[i].body, used, declared, assigned);
            }
            break;
        case NODE_WHILE:
            pfor_walk(n->as.while_stmt.cond, used, declared, assigned);
            pfor_walk_list(&n->as.while_stmt.body, used, declared, assigned);
            break;
        case NODE_FOR_RANGE:
            nameset_add(declared, n->as.for_range.var_name);
            pfor_walk(n->as.for_range.start, used, declared, assigned);
            pfor_walk(n->as.for_range.end, used, declared, assigned);
            pfor_walk(n->as.for_range.step, used, declared, assigned);
            pfor_walk_list(&n->as.for_range.body, used, declared, assigned);
            break;
        case NODE_PARALLEL_FOR:
            pfor_fail(n, "a parallel for can't be nested inside another one%s", "");
            break;
        case NODE_RETURN:
            pfor_fail(n, "'return' is not allowed in the body%s", "");
            break;
        case NODE_EXPR_STMT: pfor_walk(n->as.expr_stmt.expr, used, declared, assigned); break;
        default: break;
    }
}

static void emit_parallel_for(StrBuf *sb, Node *n, int depth) {
    if (!g_parallel_imported) {
        pfor_fail(n, "needs 'import hermes_parallel'%s", "");
    }
    int id = ++g_pfor_count;
    const char *var = n->as.parallel_for.var_name;
    NodeList *body = &n->as.parallel_for.body;

    /* split the body: local/global declarations (top level only) and the
     * statements of each iteration */
    NodeList decls = {0}, stmts = {0};
    for (int i = 0; i < body->count; i++) {
        Node *item = body->items[i];
        if (item->kind == NODE_VAR_DECL && item->as.var_decl.pscope) node_list_push(&decls, item);
        else node_list_push(&stmts, item);
    }

    /* what the iterations use from outside: identifiers that are locals
     * (or parameters) of the enclosing function, not declared inside */
    NameSet used = {0}, declared = {0}, assigned = {0};
    pfor_walk_list(&stmts, &used, &declared, &assigned);
    for (int i = 0; i < decls.count; i++) {
        Node *d = decls.items[i];
        /* a global's initializer runs in the enclosing function: not used
         * from the chunk; a local's runs in each thread */
        if (d->as.var_decl.pscope == PSCOPE_LOCAL) pfor_walk(d->as.var_decl.init, &used, &declared, &assigned);
        if (nameset_has(&declared, d->as.var_decl.name) || strcmp(d->as.var_decl.name, var) == 0) {
            pfor_fail(d, "'%s' declared twice", d->as.var_decl.name);
        }
        nameset_add(&declared, d->as.var_decl.name);
        if (d->as.var_decl.pscope == PSCOPE_GLOBAL && scope_lookup(d->as.var_decl.name)) {
            pfor_fail(d, "global '%s' already exists in the function (use another name)", d->as.var_decl.name);
        }
        if (d->as.var_decl.reduce && !d->as.var_decl.init) {
            pfor_fail(d, "reduction global '%s' needs an initial value", d->as.var_decl.name);
        }
    }
    NameSet captured = {0};
    for (int i = 0; i < used.count; i++) {
        const char *name = used.items[i];
        if (strcmp(name, var) == 0 || nameset_has(&declared, name)) continue;
        if (scope_lookup(name)) nameset_add(&captured, name);
    }
    for (int i = 0; i < captured.count; i++) {
        if (nameset_has(&assigned, captured.items[i])) {
            pfor_fail(n, "'%s' is a variable of the enclosing function: inside the loop it's a "
                         "read-only copy (to write outwards, declare it with 'global')", captured.items[i]);
        }
    }

    /* --- in the enclosing function: the globals, then the call */
    for (int i = 0; i < decls.count; i++) {
        Node *d = decls.items[i];
        if (d->as.var_decl.pscope != PSCOPE_GLOBAL) continue;
        g_in_parallel_body = 1;
        emit_stmt(sb, d, depth); /* declared + pushed into the enclosing scope */
        g_in_parallel_body = 0;
    }
    indent(sb, depth);
    sb_append(sb, "{\n");
    indent(sb, depth + 1);
    sb_appendf(sb, "struct hepy_pfor_%d hepy_pf;\n", id);
    for (int i = 0; i < decls.count; i++) {
        Node *d = decls.items[i];
        if (d->as.var_decl.pscope != PSCOPE_GLOBAL) continue;
        indent(sb, depth + 1);
        sb_appendf(sb, "hepy_pf.%s = &%s;\n", d->as.var_decl.name, d->as.var_decl.name);
    }
    for (int i = 0; i < captured.count; i++) {
        indent(sb, depth + 1);
        sb_appendf(sb, "hepy_pf.%s = %s;\n", captured.items[i], ident_text(captured.items[i]));
    }
    indent(sb, depth + 1);
    sb_append(sb, "hermes_parallel_run(");
    if (n->as.parallel_for.start) emit_expr(sb, n->as.parallel_for.start); else sb_append(sb, "0");
    sb_append(sb, ", ");
    emit_expr(sb, n->as.parallel_for.end);
    sb_append(sb, ", ");
    if (n->as.parallel_for.step) emit_expr(sb, n->as.parallel_for.step); else sb_append(sb, "1");
    sb_append(sb, ", ");
    if (n->as.parallel_for.threads) emit_expr(sb, n->as.parallel_for.threads); else sb_append(sb, "0");
    sb_appendf(sb, ", hepy_pfor_%d, &hepy_pf);\n", id);
    indent(sb, depth);
    sb_append(sb, "}\n");

    /* --- the chunk function, into g_pre */
    StrBuf *pre = &g_pre;
    emit_line(pre, n->line);
    sb_appendf(pre, "struct hepy_pfor_%d {\n    char hepy_unused;\n", id);
    for (int i = 0; i < decls.count; i++) {
        Node *d = decls.items[i];
        if (d->as.var_decl.pscope != PSCOPE_GLOBAL) continue;
        sb_appendf(pre, "    %s *%s;\n", scope_lookup(d->as.var_decl.name), d->as.var_decl.name);
    }
    for (int i = 0; i < captured.count; i++) {
        sb_appendf(pre, "    %s %s;\n", scope_lookup(captured.items[i]), captured.items[i]);
    }
    sb_append(pre, "};\n\n");
    sb_appendf(pre, "static void hepy_pfor_%d(long hepy_lo, long hepy_hi, long hepy_step, void *hepy_vctx) {\n", id);
    sb_appendf(pre, "    struct hepy_pfor_%d *hepy_ctx = hepy_vctx;\n    (void)hepy_ctx;\n", id);

    int scope_mark = g_scope_count;
    int rename_mark = g_rename_count;
    for (int i = 0; i < captured.count; i++) {
        const char *ctype = scope_lookup(captured.items[i]);
        sb_appendf(pre, "    %s const %s = hepy_ctx->%s;\n", ctype, captured.items[i], captured.items[i]);
    }
    for (int i = 0; i < decls.count; i++) {
        Node *d = decls.items[i];
        if (d->as.var_decl.pscope != PSCOPE_GLOBAL) continue;
        const char *name = d->as.var_decl.name, *op = d->as.var_decl.reduce;
        const char *ctype = scope_lookup(name);
        if (!op) {
            if (g_rename_count >= MAX_RENAMES) pfor_fail(d, "too many globals%s", "");
            char *expr = malloc(strlen(name) + 16);
            sprintf(expr, "(*hepy_ctx->%s)", name);
            g_renames[g_rename_count].name = name;
            g_renames[g_rename_count].expr = expr;
            g_rename_count++;
        } else if (strcmp(op, "+") == 0 || strcmp(op, "*") == 0) {
            sb_appendf(pre, "    %s %s = %s;\n", ctype, name, op[0] == '+' ? "0" : "1");
        } else {
            sb_appendf(pre, "    %s %s;\n", ctype, name);
            sb_appendf(pre, "    hermes_parallel_lock();\n    %s = *hepy_ctx->%s;\n    hermes_parallel_unlock();\n",
                       name, name);
        }
    }
    g_in_parallel_body = 1;
    for (int i = 0; i < decls.count; i++) {
        if (decls.items[i]->as.var_decl.pscope == PSCOPE_LOCAL) emit_stmt(pre, decls.items[i], 1);
    }
    g_in_parallel_body = 0;
    emit_line(pre, n->line);
    sb_appendf(pre, "    for (long %s = hepy_lo; %s < hepy_hi; %s += hepy_step) {\n", var, var, var);
    emit_block(pre, &stmts, 2);
    sb_append(pre, "    }\n");

    int has_reduce = 0;
    for (int i = 0; i < decls.count; i++) if (decls.items[i]->as.var_decl.reduce) has_reduce = 1;
    if (has_reduce) {
        sb_append(pre, "    hermes_parallel_lock();\n");
        for (int i = 0; i < decls.count; i++) {
            const char *name = decls.items[i]->as.var_decl.name, *op = decls.items[i]->as.var_decl.reduce;
            if (!op) continue;
            if (strcmp(op, "+") == 0) sb_appendf(pre, "    *hepy_ctx->%s += %s;\n", name, name);
            else if (strcmp(op, "*") == 0) sb_appendf(pre, "    *hepy_ctx->%s *= %s;\n", name, name);
            else sb_appendf(pre, "    if (%s %s *hepy_ctx->%s) *hepy_ctx->%s = %s;\n",
                            name, strcmp(op, "min") == 0 ? "<" : ">", name, name, name);
        }
        sb_append(pre, "    hermes_parallel_unlock();\n");
    }
    sb_append(pre, "}\n\n");

    g_scope_count = scope_mark;
    g_rename_count = rename_mark;
    free(used.items); free(declared.items); free(assigned.items); free(captured.items);
    free(decls.items); free(stmts.items);
}

static void emit_func_prototype(StrBuf *sb, Node *fn) {
    emit_line(sb, fn->line);
    const char *ret = fn->as.func_def.return_type ? fn->as.func_def.return_type : "void";
    sb_appendf(sb, "%s %s(", ret, fn->as.func_def.name);
    NodeList *params = &fn->as.func_def.params;
    if (params->count == 0) {
        sb_append(sb, "void");
    } else {
        for (int i = 0; i < params->count; i++) {
            if (i > 0) sb_append(sb, ", ");
            Node *param = params->items[i];
            /* A `device` parameter has no recognizable constructor call to
             * infer a kind from, so it always falls back to the generic
             * handle -- usable for device_name()/device_kind_name(), not for
             * .method() (see the device-kind tracking comment above). */
            const char *type_name = strcmp(param->as.param.type_name, "device") == 0
                ? "hermes_handle_t" : param->as.param.type_name;
            sb_appendf(sb, "%s %s", type_name, param->as.param.name);
        }
    }
    sb_append(sb, ")");
}

static void emit_func_def(StrBuf *sb, Node *fn) {
    StrBuf body;
    sb_init(&body);
    if (!g_pre_ready) { sb_init(&g_pre); g_pre_ready = 1; }
    g_pre.len = 0;
    g_pre.data[0] = '\0';

    emit_func_prototype(&body, fn);
    sb_append(&body, " {\n");
    g_local_device_count = 0; /* fresh scope: forget the previous function's device vars */
    g_emitting_function_body = 1;
    g_scope_count = 0;
    NodeList *params = &fn->as.func_def.params;
    for (int i = 0; i < params->count; i++) {
        Node *param = params->items[i];
        scope_push(param->as.param.name, strcmp(param->as.param.type_name, "device") == 0
                   ? "hermes_handle_t" : param->as.param.type_name);
    }
    emit_block(&body, &fn->as.func_def.body, 1);
    g_emitting_function_body = 0;
    /* the closing brace: at the def's line (what gcc reports for "the
     * function can end without return"); without it, gcc would keep counting
     * from the last statement and land on some later line */
    emit_line(&body, fn->line);
    sb_append(&body, "}\n\n");

    sb_append(sb, g_pre.data); /* this function's parallel for chunks */
    sb_append(sb, body.data);
    free(body.data);
}

char *codegen_generate(Node *program, const char *source_path) {
    StrBuf sb;
    sb_init(&sb);

    if (source_path) {
        g_source_display = source_path;
        /* escape for a C string literal: \ and " */
        char *esc = malloc(strlen(source_path) * 2 + 1), *o = esc;
        for (const char *c = source_path; *c; c++) {
            if (*c == '\\' || *c == '"') *o++ = '\\';
            *o++ = *c;
        }
        *o = '\0';
        g_source_path = esc;
    }

    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind == NODE_IMPORT && strcmp(item->as.import_stmt.module_name, "hermes_parallel") == 0) {
            g_parallel_imported = 1;
        }
    }

    /* Top-level items in the order of the .hepy, so an insert_C block
     * lands exactly where it was written (if the file starts with one,
     * its C is the first thing in the .c). Only two things are added:
     * the header (comment + stdint/stdbool), before the first item that
     * isn't an insert_C, and the prototypes of every function, right
     * before the first `def` (so functions can call each other in any
     * order). */
    int header_done = 0, prototypes_done = 0;
    NodeKind prev = NODE_PROGRAM;
    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (!header_done && item->kind != NODE_INSERT_C) {
            sb_append(&sb, "/* Generated by hepyc. Do not edit by hand. */\n");
            sb_append(&sb, "#include <stdint.h>\n");
            sb_append(&sb, "#include <stdbool.h>\n");
            header_done = 1;
        }
        if (prev == NODE_IMPORT && item->kind != NODE_IMPORT) sb_append(&sb, "\n");
        switch (item->kind) {
            case NODE_IMPORT: {
                /* architecture and board markers (e.g. `import avr`, `import
                 * mega`) and thread-size profiles (e.g. `import light`) are
                 * build directives consumed by hepyc's driver, not real C
                 * headers -- see arch.h/board.h/threadprofile.h */
                const char *mod = item->as.import_stmt.module_name;
                if (hermes_arch_lookup(mod) != NULL || hermes_board_find(mod) != NULL ||
                    hermes_threadprofile_lookup(mod) != NULL) break;
                /* a module (modules/<name>/, already loaded by main.c before
                 * codegen) is included through the modules dir, which hepyc
                 * adds as -I: `#include "<name>/<header>"` */
                const HermesModule *m = hermes_module_find(mod);
                if (m) sb_appendf(&sb, "#include \"%s/%s\"\n", m->name, m->header);
                else sb_appendf(&sb, "#include \"%s.h\"\n", mod);
                break;
            }
            case NODE_VAR_DECL: /* global variable: must have a constant initializer, if any */
            case NODE_INSERT_C:
                emit_stmt(&sb, item, 0);
                break;
            case NODE_FUNC_DEF:
                if (!prototypes_done) {
                    sb_append(&sb, "\n");
                    for (int j = 0; j < program->as.program.items.count; j++) {
                        Node *fn = program->as.program.items.items[j];
                        if (fn->kind != NODE_FUNC_DEF) continue;
                        emit_func_prototype(&sb, fn);
                        sb_append(&sb, ";\n");
                    }
                    sb_append(&sb, "\n");
                    prototypes_done = 1;
                }
                emit_func_def(&sb, item);
                break;
            default:
                break;
        }
        prev = item->kind;
    }

    return sb.data;
}
