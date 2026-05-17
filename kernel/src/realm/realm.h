#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../lib/list.h"
#include "../mm/vmm.h"

// ---------------------------------------------------------------------------
//  realm/realm.h — Realm kernel object (Phase 4)
//
//  A Realm is a first-class kernel object: an isolated execution territory
//  with its own address space, task list, type tag, and capability table.
//  The kernel creates, schedules, and destroys Realms from Ring 0.
// ---------------------------------------------------------------------------

struct task;  // forward declaration — avoids circular include with sched.h

// ── Realm types ───────────────────────────────────────────────────────────
typedef enum {
    REALM_NATIVE = 0,
    REALM_WASM   = 1,
    REALM_LINUX  = 2,
    REALM_WIN32  = 3,
} realm_type_t;

// ── Realm states ──────────────────────────────────────────────────────────
typedef enum {
    REALM_CREATED  = 0,
    REALM_RUNNING  = 1,
    REALM_STOPPED  = 2,
    REALM_DEAD     = 3,
} realm_state_t;

// ── Capability bits ───────────────────────────────────────────────────────
#define CAP_VFS          (1u << 0)
#define CAP_IPC          (1u << 1)
#define CAP_PAGES        (1u << 2)
#define CAP_IRQ          (1u << 3)
#define CAP_GPU          (1u << 4)
#define CAP_NETWORK      (1u << 5)
#define CAP_LIBOS_MAP    (1u << 6)
#define CAP_REALM_CREATE (1u << 7)
#define CAP_POWER        (1u << 8)

// Default capabilities for application realms
#define CAP_DEFAULT  (CAP_VFS | CAP_PAGES)

// ── Limits ────────────────────────────────────────────────────────────────
#define REALM_MAX_TASKS  32
#define REALM_NAME_MAX   32
#define MAX_REALMS       64
#define LIBOS_MODULE_NAME_MAX 32
#define LIBOS_MODULE_PATH_MAX 80
#define LIBOS_MAX_MODULES     16

// ── Realm kernel object ──────────────────────────────────────────────────
typedef struct realm {
    uint32_t         id;
    char             name[REALM_NAME_MAX];
    realm_type_t     type;
    realm_state_t    state;
    uint32_t         caps;              // capability bitmask

    page_table_t    *page_table;        // isolated PML4
    struct task     *tasks[REALM_MAX_TASKS];
    uint32_t         task_count;

    uint64_t         page_count;        // pages currently granted
    uint64_t         page_quota;        // max pages allowed (0 = unlimited)

    list_node_t      list;              // global realm list node
} realm_t;

typedef struct {
    uint32_t     id;
    realm_type_t type;
    char         name[LIBOS_MODULE_NAME_MAX];
    char         path[LIBOS_MODULE_PATH_MAX];
    uint64_t     size;
    uint32_t     flags;
} libos_module_t;

// ── Lifecycle API ─────────────────────────────────────────────────────────
void     realm_system_init(void);
realm_t *realm_create(realm_type_t type, const char *name);
int      realm_destroy(uint32_t id);
int      realm_destroy_current(void);
struct task *realm_exec(realm_t *r, const void *binary, size_t size,
                        const char *task_name, uint32_t cpu_affinity);
int      realm_detect_binary(const void *binary, size_t size, realm_type_t *type);
realm_t *realm_create_for_binary(const void *binary, size_t size,
                                 const char *name);

// ── Queries ───────────────────────────────────────────────────────────────
realm_t *realm_find(uint32_t id);
realm_t *realm_current(void);

// ── Task association ──────────────────────────────────────────────────────
int  realm_add_task(realm_t *r, struct task *t);
void realm_remove_task(realm_t *r, struct task *t);

// ── LibOS init (Phase 4 placeholder) ──────────────────────────────────────
void libos_init(void);
const libos_module_t *libos_fetch_module(realm_type_t type, const char *name);
const libos_module_t *libos_module_at(size_t idx);
size_t libos_module_count(void);
