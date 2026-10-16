/* Virtual HC-05 / HM-10: see bt_virtual.h.
 * HM-10 (firmware V5xx/V7xx): answers without line endings; while not
 * connected, "AT" -> "OK", "AT+NAME<n>" -> "OK+Set:<n>", "AT+NOTI1" ->
 * "OK+Set:1" (and then a central connects: "OK+CONN"); anything else,
 * silence. A command ends with a pause in the bytes, as on the real
 * module: here, QUIET polls of the output with nothing new. */
#include "bt_virtual.h"
#include <stdlib.h>
#include <string.h>

#define OUT 4096
#define QUIET 100 /* polls with no new byte = the pause that ends a command */

struct btv {
    int hm10, connected, notify;
    char cmd[40];
    int len, idle;
    uint8_t out[OUT];
    unsigned head, tail;
};

static void out_bytes(btv_t *v, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        unsigned next = (v->head + 1) % OUT;
        if (next == v->tail) return;
        v->out[v->head] = b[i];
        v->head = next;
    }
}
static void out_str(btv_t *v, const char *s) { out_bytes(v, s, strlen(s)); }

btv_t *btv_new_hc05(void) {
    btv_t *v = calloc(1, sizeof(*v));
    v->connected = 1;
    return v;
}

btv_t *btv_new_hm10(void) {
    btv_t *v = calloc(1, sizeof(*v));
    v->hm10 = 1;
    return v;
}

int btv_connected(btv_t *v) { return v->connected; }

/* an HM-10 command, complete */
static void hm10_command(btv_t *v) {
    v->cmd[v->len] = 0;
    if (!strcmp(v->cmd, "AT")) out_str(v, "OK");
    else if (!strncmp(v->cmd, "AT+NAME", 7)) { out_str(v, "OK+Set:"); out_str(v, v->cmd + 7); }
    else if (!strcmp(v->cmd, "AT+NOTI1")) {
        out_str(v, "OK+Set:1");
        v->notify = 1;
        v->connected = 1; /* a central (a phone) connects now */
        out_str(v, "OK+CONN");
    }
    v->len = 0;
}

void btv_input(btv_t *v, uint8_t b) {
    if (v->connected) { /* the remote device echoes it back */
        out_bytes(v, &b, 1);
        return;
    }
    if (!v->hm10) return;
    v->idle = 0;
    if (v->len < (int)sizeof(v->cmd) - 1) v->cmd[v->len++] = (char)b;
}

int btv_output(btv_t *v) {
    /* the HM-10 takes a pause in the bytes as a command's end */
    if (v->len && ++v->idle >= QUIET) hm10_command(v);
    if (v->tail == v->head) return -1;
    int b = v->out[v->tail];
    v->tail = (v->tail + 1) % OUT;
    return b;
}
