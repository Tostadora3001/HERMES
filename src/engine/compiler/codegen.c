#include "codegen.h"
#include "arch.h"
#include "mempool.h"
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

static void emit_expr(StrBuf *sb, Node *n);
static void emit_block(StrBuf *sb, NodeList *block, int depth);
static void emit_stmt(StrBuf *sb, Node *n, int depth);

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
 * generic `hermes_device_t *` and simply can't be used with `.method()`
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
            sb_append(sb, n->as.ident.name);
            break;
        case NODE_CALL:
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
                    "codegen (linea %d): solo se puede llamar a un metodo sobre una variable "
                    "directamente (p. ej. 'x.metodo(...)'), no sobre una expresion\n", n->line);
                exit(1);
            }
            const char *kind = device_var_lookup(recv->as.ident.name);
            if (!kind) {
                fprintf(stderr,
                    "codegen (linea %d): no se puede resolver el tipo de dispositivo de '%s' para "
                    "llamar a '.%s(...)' -- solo funciona sobre una variable 'device' declarada con "
                    "un constructor de dispositivo reconocido (ver src/engine/compiler/devicekind.c); "
                    "los parametros 'device' genericos no soportan llamadas a metodo\n",
                    n->line, recv->as.ident.name, n->as.method_call.method_name);
                exit(1);
            }
            sb_appendf(sb, "device_%s_%s(%s", kind, n->as.method_call.method_name, recv->as.ident.name);
            if (n->as.method_call.args.count > 0) {
                sb_append(sb, ", ");
                emit_call_args(sb, &n->as.method_call.args);
            }
            sb_append(sb, ")");
            break;
        }
        default:
            fprintf(stderr, "codegen: nodo de expresion inesperado (kind=%d)\n", n->kind);
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
                if (n->as.var_decl.init && n->as.var_decl.init->kind == NODE_CALL) {
                    dk = hermes_devicekind_lookup_constructor(n->as.var_decl.init->as.call.name);
                }
                type_name = dk ? dk->c_type : "hermes_device_t *";
                if (dk) device_var_register(n->as.var_decl.name, dk->kind);
            }
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
            sb_appendf(sb, "%s = ", n->as.assign.name);
            emit_expr(sb, n->as.assign.value);
            sb_append(sb, ";\n");
            break;
        case NODE_COMPOUND_ASSIGN:
            indent(sb, depth);
            sb_appendf(sb, "%s %c= ", n->as.compound_assign.name, n->as.compound_assign.op);
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
            emit_block(sb, &n->as.for_range.body, depth + 1);
            indent(sb, depth);
            sb_append(sb, "}\n");
            break;
        }
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
            fprintf(stderr, "codegen: instruccion inesperada (kind=%d)\n", n->kind);
            exit(1);
    }
}

static void emit_block(StrBuf *sb, NodeList *block, int depth) {
    for (int i = 0; i < block->count; i++) {
        emit_stmt(sb, block->items[i], depth);
    }
}

static void emit_func_prototype(StrBuf *sb, Node *fn) {
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
             * base type -- usable for device_name()/device_kind_name(), not for
             * .method() (see the device-kind tracking comment above). */
            const char *type_name = strcmp(param->as.param.type_name, "device") == 0
                ? "hermes_device_t *" : param->as.param.type_name;
            sb_appendf(sb, "%s %s", type_name, param->as.param.name);
        }
    }
    sb_append(sb, ")");
}

static void emit_func_def(StrBuf *sb, Node *fn) {
    emit_func_prototype(sb, fn);
    sb_append(sb, " {\n");
    g_local_device_count = 0; /* fresh scope: forget the previous function's device vars */
    g_emitting_function_body = 1;
    emit_block(sb, &fn->as.func_def.body, 1);
    g_emitting_function_body = 0;
    sb_append(sb, "}\n\n");
}

char *codegen_generate(Node *program) {
    StrBuf sb;
    sb_init(&sb);

    sb_append(&sb, "/* Generado automaticamente por hepyc. No editar a mano. */\n");
    sb_append(&sb, "#include <stdint.h>\n");
    sb_append(&sb, "#include <stdbool.h>\n");

    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind == NODE_IMPORT) {
            /* architecture markers (e.g. `import avr`) and memory-profile
             * markers (e.g. `import light`) are build directives consumed
             * by hepyc's driver, not real C headers -- see arch.h/mempool.h */
            const char *mod = item->as.import_stmt.module_name;
            if (hermes_arch_lookup(mod) != NULL || hermes_mempool_lookup(mod) != NULL) continue;
            /* a module (modules/<name>/, already loaded by main.c before
             * codegen) is included through the modules dir, which hepyc
             * adds as -I: `#include "<name>/<header>"` */
            const HermesModule *m = hermes_module_find(mod);
            if (m) sb_appendf(&sb, "#include \"%s/%s\"\n", m->name, m->header);
            else sb_appendf(&sb, "#include \"%s.h\"\n", mod);
        }
    }
    sb_append(&sb, "\n");

    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind == NODE_VAR_DECL) {
            emit_stmt(&sb, item, 0); /* global variable: must have a constant initializer, if any */
        }
    }
    sb_append(&sb, "\n");

    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind == NODE_FUNC_DEF) {
            emit_func_prototype(&sb, item);
            sb_append(&sb, ";\n");
        }
    }
    sb_append(&sb, "\n");

    for (int i = 0; i < program->as.program.items.count; i++) {
        Node *item = program->as.program.items.items[i];
        if (item->kind == NODE_FUNC_DEF) {
            emit_func_def(&sb, item);
        }
    }

    return sb.data;
}
