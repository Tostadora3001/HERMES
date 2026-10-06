#include "ast.h"
#include <stdlib.h>
#include <string.h>

Node *node_new(NodeKind kind, int line) {
    Node *n = calloc(1, sizeof(Node));
    n->kind = kind;
    n->line = line;
    return n;
}

void node_list_push(NodeList *list, Node *node) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 4;
        list->items = realloc(list->items, sizeof(Node *) * list->capacity);
    }
    list->items[list->count++] = node;
}

void if_branch_list_push(IfBranchList *list, IfBranch branch) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 4;
        list->items = realloc(list->items, sizeof(IfBranch) * list->capacity);
    }
    list->items[list->count++] = branch;
}
