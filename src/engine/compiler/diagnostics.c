#define _XOPEN_SOURCE 700 /* popen, pclose */
#include "diagnostics.h"
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* --- the rewrite table ------------------------------------------------------
 * Applied to the message part of a compiler line ("<where>: <kind>: <message>"),
 * first match wins. `to` may use \1..\3 for the pattern's groups; NULL drops
 * the whole line (notes that only add noise for a HERMESPY program).
 * Patterns are POSIX extended regexes over gcc's English messages (LC_ALL=C);
 * both gcc and avr-gcc 7.3 word them this way. Add an entry when a common
 * mistake produces a C-flavoured message. */
typedef struct {
    const char *pattern;
    const char *to;
} Rewrite;

static const Rewrite REWRITES[] = {
    /* names */
    { "^'([A-Za-z_][A-Za-z0-9_]*)' undeclared \\(first use in this function\\)$",
      "variable '\\1' is not declared (declare it first, e.g. 'long \\1 = 0')" },
    { "^'([A-Za-z_][A-Za-z0-9_]*)' undeclared here \\(not in a function\\)$",
      "'\\1' is not declared" },
    { "^each undeclared identifier is reported only once for each function it appears in$", NULL },
    { "^implicit declaration of function '([A-Za-z_][A-Za-z0-9_]*)'.*$",
      "call to unknown function '\\1' (not defined in this file nor provided by any import)" },
    { "^unknown type name '([A-Za-z_][A-Za-z0-9_]*)'$",
      "unknown type '\\1' (types are C types such as long, double, bool, or handle types such as hermes_sem_handle_t)" },
    { "^called object '([A-Za-z_][A-Za-z0-9_]*)' is not a function or function pointer$",
      "'\\1' is a variable, not a function: it can't be called" },
    { "^called object is not a function or function pointer$",
      "this is not a function: it can't be called" },

    /* definitions */
    { "^redefinition of '([A-Za-z_][A-Za-z0-9_]*)'$", "'\\1' is defined twice" },
    { "^'([A-Za-z_][A-Za-z0-9_]*)' redeclared as different kind of symbol$",
      "'\\1' is already used as another kind of name (function/variable)" },
    { "^conflicting types for '([A-Za-z_][A-Za-z0-9_]*)'.*$",
      "'\\1' is declared with two different types" },
    { "^previous (definition|declaration) of '([A-Za-z_][A-Za-z0-9_]*)'.*$",
      "'\\2' was first defined here" },
    { "^initializer element is not constant$",
      "a global variable's initial value must be a constant (assign it inside a function instead, e.g. in main)" },

    /* print */
    { "^'_Generic' selector of type '(.*)' is not compatible with any association$",
      "print() can't print a value of type '\\1' (it prints integers, decimals, booleans and text)" },
    { "^in expansion of macro 'print'$", "in this call to print()" },

    /* calls and values */
    { "^too few arguments to function '([A-Za-z_][A-Za-z0-9_]*)'$",
      "too few arguments in the call to '\\1'" },
    { "^too many arguments to function '([A-Za-z_][A-Za-z0-9_]*)'$",
      "too many arguments in the call to '\\1'" },
    { "^passing argument ([0-9]+) of '([A-Za-z_][A-Za-z0-9_]*)' makes integer from pointer without a cast.*$",
      "argument \\1 of '\\2' must be a number, but a handle or text was given" },
    { "^passing argument ([0-9]+) of '([A-Za-z_][A-Za-z0-9_]*)' makes pointer from integer without a cast.*$",
      "argument \\1 of '\\2' must be a handle (device, semaphore...) or text, but a number was given" },
    { "^passing argument ([0-9]+) of '([A-Za-z_][A-Za-z0-9_]*)' from incompatible pointer type.*$",
      "argument \\1 of '\\2' has the wrong kind of handle or function" },
    { "^incompatible types when assigning to type '(.*)' from type '(.*)'$",
      "can't assign a value of type '\\2' to a variable of type '\\1'" },
    { "^incompatible types when initializing type '(.*)' using type '(.*)'$",
      "can't initialize a variable of type '\\1' with a value of type '\\2'" },
    /* gcc >= 8 names both types ("assignment to 'X' from 'Y' makes ..."),
     * avr-gcc 7.3 doesn't ("assignment makes ...") */
    { "^assignment( to '.*' from '.*')? makes integer from pointer without a cast.*$",
      "assigning a handle or text to a number variable" },
    { "^assignment( to '.*' from '.*')? makes pointer from integer without a cast.*$",
      "assigning a number to a handle variable" },
    { "^initialization( of '.*' from '.*')? makes integer from pointer without a cast.*$",
      "initializing a number variable with a handle or text" },
    { "^initialization( of '.*' from '.*')? makes pointer from integer without a cast.*$",
      "initializing a handle variable with a number" },
    { "^expected '(.*)' but argument is of type '(.*)'$",
      "expected a value of type '\\1', got '\\2'" },
    { "^comparison between pointer and integer$", "comparing a handle with a number" },
    { "^assignment of read-only variable '([A-Za-z_][A-Za-z0-9_]*)'$",
      "'\\1' is read-only: it can't be assigned" },
    { "^void value not ignored as it ought to be$",
      "this function returns nothing (-> void): its result can't be used" },

    /* warnings */
    { "^unused variable '([A-Za-z_][A-Za-z0-9_]*)'.*$", "variable '\\1' is never used" },
    { "^unused parameter '([A-Za-z_][A-Za-z0-9_]*)'.*$", "parameter '\\1' is never used" },
    { "^'([A-Za-z_][A-Za-z0-9_]*)' is used uninitialized.*$",
      "variable '\\1' is used before it gets a value" },
    { "^'([A-Za-z_][A-Za-z0-9_]*)' may be used uninitialized.*$",
      "variable '\\1' may be used before it gets a value" },
    { "^control reaches end of non-void function.*$",
      "the function can end without 'return' (it declares a return type)" },
    { "^'return' with no value, in function returning non-void.*$",
      "'return' without a value in a function that declares a return type" },
    { "^'return' with a value, in function returning void.*$",
      "'return' with a value in a function without a return type (-> void)" },
    { "^division by zero.*$", "division by zero" },
    { "^suggest parentheses around .*$", NULL },
};
#define REWRITE_COUNT ((int)(sizeof(REWRITES) / sizeof(REWRITES[0])))

/* C type names as HERMESPY shows them (applied to the rewritten message). */
static const struct { const char *c; const char *hepy; } TYPE_NAMES[] = {
    { "struct hermes_thread *", "thread handle" },
    { "hermes_thread_t *", "thread handle" },
    { "hermes_thread_handle_t", "thread handle" },
    { "struct hermes_sem *", "semaphore handle" },
    { "hermes_sem_t *", "semaphore handle" },
    { "hermes_sem_handle_t", "semaphore handle" },
    { "hermes_device_t *", "device" },
    { "struct hermes_device *", "device" },
    { "void (*)(long int)", "function (long) -> void" },
    { "long int", "long" },
    { "_Bool", "bool" },
    { "const char *", "text" },
    { "char *", "text" },
};

static regex_t g_regex[REWRITE_COUNT];
static int g_compiled = 0;

static void compile_table(void) {
    if (g_compiled) return;
    for (int i = 0; i < REWRITE_COUNT; i++) {
        if (regcomp(&g_regex[i], REWRITES[i].pattern, REG_EXTENDED) != 0) {
            fprintf(stderr, "hepyc: internal error: bad diagnostic pattern %d\n", i);
            exit(1);
        }
    }
    g_compiled = 1;
}

static char *replace_all(const char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *p = strstr(s, from); p; p = strstr(p + fl, from)) n++;
    char *out = malloc(strlen(s) + n * (tl > fl ? tl - fl : 0) + 1), *o = out;
    for (const char *p = s;;) {
        const char *q = strstr(p, from);
        if (!q) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, to, tl); o += tl;
        p = q + fl;
    }
    return out;
}

/* NULL = drop the line */
static char *rewrite_message(const char *msg) {
    regmatch_t m[4];
    for (int i = 0; i < REWRITE_COUNT; i++) {
        if (regexec(&g_regex[i], msg, 4, m, 0) != 0) continue;
        if (!REWRITES[i].to) return NULL;
        size_t cap = strlen(REWRITES[i].to) + strlen(msg) * 3 + 1;
        char *out = malloc(cap), *o = out;
        for (const char *t = REWRITES[i].to; *t; t++) {
            if (t[0] == '\\' && t[1] >= '1' && t[1] <= '3' && m[t[1] - '0'].rm_so >= 0) {
                int g = t[1] - '0';
                size_t len = (size_t)(m[g].rm_eo - m[g].rm_so);
                memcpy(o, msg + m[g].rm_so, len); o += len;
                t++;
            } else {
                *o++ = *t;
            }
        }
        *o = '\0';
        for (size_t k = 0; k < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]); k++) {
            char *r = replace_all(out, TYPE_NAMES[k].c, TYPE_NAMES[k].hepy);
            free(out);
            out = r;
        }
        return out;
    }
    return strdup(msg);
}

char *hermes_translate_diagnostic(const char *line) {
    compile_table();
    static const char *const KINDS[] = { ": error: ", ": warning: ", ": note: ", ": fatal error: " };
    for (size_t k = 0; k < sizeof(KINDS) / sizeof(KINDS[0]); k++) {
        const char *at = strstr(line, KINDS[k]);
        if (!at) continue;
        const char *msg = at + strlen(KINDS[k]);
        /* keep gcc's "[-Wflag]" at the end out of the match */
        size_t mlen = strlen(msg);
        while (mlen && (msg[mlen - 1] == '\n' || msg[mlen - 1] == '\r')) mlen--;
        const char *flag = NULL;
        if (mlen && msg[mlen - 1] == ']') {
            const char *open = NULL;
            for (const char *p = msg; p < msg + mlen; p++) if (p[0] == ' ' && p[1] == '[') open = p;
            if (open) { flag = open; mlen = (size_t)(open - msg); }
        }
        char *core = strndup(msg, mlen);
        /* gcc's " {aka 'struct hermes_sem *'}" (avr-gcc 7.3: inside the
         * quotes, " {aka struct hermes_sem *}"): the C spelling of a type
         * already named -- noise in HERMESPY terms */
        for (char *aka; (aka = strstr(core, " {aka ")) != NULL;) {
            char *end = strchr(aka, '}');
            if (!end) break;
            memmove(aka, end + 1, strlen(end + 1) + 1);
        }
        char *rewritten = rewrite_message(core);
        free(core);
        if (!rewritten) return NULL;
        size_t head = (size_t)(msg - line);
        size_t flen = flag ? strlen(flag) : 0;
        while (flen && (flag[flen - 1] == '\n' || flag[flen - 1] == '\r')) flen--;
        char *out = malloc(head + strlen(rewritten) + flen + 2);
        memcpy(out, line, head);
        strcpy(out + head, rewritten);
        if (flag) strncat(out, flag, flen);
        strcat(out, "\n");
        free(rewritten);
        return out;
    }
    return strdup(line);
}

int hermes_run_compiler(const char *cmd, int raw) {
    size_t len = strlen(cmd) + 32;
    char *full = malloc(len);
    snprintf(full, len, "LC_ALL=C %s 2>&1", cmd);
    FILE *p = popen(full, "r");
    free(full);
    if (!p) { perror("hepyc: popen"); return 1; }
    char line[4096];
    int dropping_context = 0;
    while (fgets(line, sizeof(line), p)) {
        if (raw) { fputs(line, stderr); continue; }
        /* a dropped note's source snippet (" 12 | ..." / "    | ^~~") goes too */
        if (dropping_context && (line[0] == ' ')) continue;
        dropping_context = 0;
        /* "In file included from <the generated .c>:5:" (+ "   from ..."
         * lines): the chain of C headers, meaningless in HERMESPY */
        if (strncmp(line, "In file included from ", 22) == 0 ||
            strncmp(line, "                 from ", 22) == 0) continue;
        char *out = hermes_translate_diagnostic(line);
        if (!out) { dropping_context = 1; continue; }
        fputs(out, stderr);
        free(out);
    }
    int status = pclose(p);
    if (status == -1) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
