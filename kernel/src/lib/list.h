#pragma once
#include <stddef.h>

// ---------------------------------------------------------------------------
//  lib/list.h — Intrusive doubly-linked circular list
//
//  Usage:
//    struct task { list_node_t list; int pid; };
//    list_t ready_queue = LIST_INIT(ready_queue);
//    list_append(&ready_queue, &task->list);
//    list_foreach(&ready_queue, node) {
//        struct task *t = container_of(node, struct task, list);
//    }
// ---------------------------------------------------------------------------

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct list_node {
    struct list_node *next;
    struct list_node *prev;
} list_node_t;

typedef list_node_t list_t;

#define LIST_INIT(name)  { .next = &(name), .prev = &(name) }

static inline void list_init(list_t *l) {
    l->next = l;
    l->prev = l;
}

static inline int list_empty(const list_t *l) {
    return l->next == l;
}

// Insert node AFTER prev
static inline void __list_insert(list_node_t *prev,
                                 list_node_t *next,
                                 list_node_t *node) {
    node->prev = prev;
    node->next = next;
    prev->next = node;
    next->prev = node;
}

static inline void list_append(list_t *head, list_node_t *node) {
    __list_insert(head->prev, head, node);
}

static inline void list_prepend(list_t *head, list_node_t *node) {
    __list_insert(head, head->next, node);
}

static inline void list_remove(list_node_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

// Pop the first element (or NULL if empty)
static inline list_node_t *list_pop_front(list_t *head) {
    if (list_empty(head)) return 0;
    list_node_t *n = head->next;
    list_remove(n);
    return n;
}

// Iterate (safe to remove current node)
#define list_foreach(head, it) \
    for (list_node_t *it = (head)->next, *_next = it->next; \
         it != (head); \
         it = _next, _next = it->next)
