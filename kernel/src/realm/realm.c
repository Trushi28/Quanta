// ============================================================
//  realm/realm.c — Realm lifecycle manager (Phase 4)
//
//  Manages realm_t kernel objects: creation, destruction, lookup,
//  and task association.
// ============================================================
#include "realm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../lib/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../fs/vfs.h"
#include "elf.h"
#include "uspace.h"
#include <stddef.h>

// ── Global state ──────────────────────────────────────────────────────────
static spinlock_t realm_lock  = SPINLOCK_INIT;
static list_t     realm_list  = LIST_INIT(realm_list);
static uint32_t   next_realm_id = 1;
static realm_t   *realm_pool[MAX_REALMS];
static int        realm_pool_used = 0;

// LibOS realm (created at boot, id=0)
static realm_t   *libos_realm = NULL;

// ── realm_system_init ─────────────────────────────────────────────────────
void realm_system_init(void) {
    for (int i = 0; i < MAX_REALMS; i++)
        realm_pool[i] = NULL;
    realm_pool_used = 0;
    next_realm_id = 1;
    kprintf("[REALM] Realm system initialised  (max %d realms)\n", MAX_REALMS);
}

// ── realm_create ──────────────────────────────────────────────────────────
realm_t *realm_create(realm_type_t type, const char *name) {
    uint64_t rflags = spinlock_irq_acquire(&realm_lock);

    if (realm_pool_used >= MAX_REALMS) {
        spinlock_irq_release(&realm_lock, rflags);
        kprintf("[REALM] Cannot create realm: pool exhausted\n");
        return NULL;
    }

    realm_t *r = (realm_t *)kmalloc(sizeof(realm_t));
    if (!r) {
        spinlock_irq_release(&realm_lock, rflags);
        return NULL;
    }
    memset(r, 0, sizeof(realm_t));

    r->id         = next_realm_id++;
    r->type       = type;
    r->state      = REALM_CREATED;
    r->caps       = CAP_DEFAULT;
    r->page_quota = 0;  // unlimited for now
    strncpy(r->name, name ? name : "unnamed", REALM_NAME_MAX - 1);
    r->name[REALM_NAME_MAX - 1] = '\0';

    // Create isolated address space
    r->page_table = vmm_new_space();
    if (!r->page_table) {
        kfree(r);
        spinlock_irq_release(&realm_lock, rflags);
        kprintf("[REALM] Cannot create address space for realm '%s'\n", name);
        return NULL;
    }

    list_init(&r->list);
    list_append(&realm_list, &r->list);
    realm_pool[realm_pool_used++] = r;

    spinlock_irq_release(&realm_lock, rflags);

    kprintf("[REALM] Created realm #%u '%s'  type=%u  PML4=0x%llx\n",
            r->id, r->name, (unsigned)type,
            (unsigned long long)r->page_table->pml4_phys);
    return r;
}

// ── realm_destroy ─────────────────────────────────────────────────────────
int realm_destroy(uint32_t id) {
    uint64_t rflags = spinlock_irq_acquire(&realm_lock);

    realm_t *r = NULL;
    int slot = -1;
    for (int i = 0; i < realm_pool_used; i++) {
        if (realm_pool[i] && realm_pool[i]->id == id) {
            r = realm_pool[i];
            slot = i;
            break;
        }
    }

    if (!r || r->state == REALM_DEAD) {
        spinlock_irq_release(&realm_lock, rflags);
        return -1;
    }

    r->state = REALM_DEAD;

    // Kill all tasks in this realm
    for (uint32_t i = 0; i < r->task_count; i++) {
        if (r->tasks[i]) {
            // Mark zombie — scheduler will clean up
            r->tasks[i]->state = TASK_ZOMBIE;
            r->tasks[i]->realm = NULL;
            r->tasks[i]->page_table = NULL;
            r->tasks[i] = NULL;
        }
    }
    r->task_count = 0;

    list_remove(&r->list);
    if (slot >= 0) {
        for (int i = slot; i < realm_pool_used - 1; i++)
            realm_pool[i] = realm_pool[i + 1];
        realm_pool[--realm_pool_used] = NULL;
    }

    spinlock_irq_release(&realm_lock, rflags);

    uint64_t freed = vmm_destroy_space(r->page_table);
    r->page_table = NULL;
    r->page_count = 0;
    kprintf("[REALM] Destroyed realm #%u '%s'  freed=%llu pages\n",
            r->id, r->name, (unsigned long long)freed);

    kfree(r);
    return 0;
}

// ── realm_destroy_current ─────────────────────────────────────────────────
int realm_destroy_current(void) {
    realm_t *r = realm_current();
    if (!r) return -1;
    task_t *cur = sched_current();
    if (cur) {
        cur->realm = NULL;
        cur->page_table = NULL;
    }
    vmm_load(kernel_page_table);
    int rc = realm_destroy(r->id);
    // After destroying, the current task should exit
    sched_exit(-1);
    __builtin_unreachable();
    return rc;
}

// ── realm_find ────────────────────────────────────────────────────────────
realm_t *realm_find(uint32_t id) {
    uint64_t rflags = spinlock_irq_acquire(&realm_lock);
    realm_t *found = NULL;
    for (int i = 0; i < realm_pool_used; i++) {
        if (realm_pool[i] && realm_pool[i]->id == id) {
            found = realm_pool[i];
            break;
        }
    }
    spinlock_irq_release(&realm_lock, rflags);
    return found;
}

// ── realm_current ─────────────────────────────────────────────────────────
realm_t *realm_current(void) {
    task_t *cur = sched_current();
    if (!cur) return NULL;
    return (realm_t *)cur->realm;
}

// ── realm_add_task ────────────────────────────────────────────────────────
int realm_add_task(realm_t *r, struct task *t) {
    if (!r || !t) return -1;
    uint64_t rflags = spinlock_irq_acquire(&realm_lock);
    if (r->task_count >= REALM_MAX_TASKS) {
        spinlock_irq_release(&realm_lock, rflags);
        return -1;
    }
    r->tasks[r->task_count++] = (task_t *)t;
    ((task_t *)t)->realm = r;
    ((task_t *)t)->page_table = r->page_table;
    spinlock_irq_release(&realm_lock, rflags);
    return 0;
}

struct task *realm_exec(realm_t *r, const void *binary, size_t size,
                        const char *task_name, uint32_t cpu_affinity) {
    if (!r || !binary || !size)
        return NULL;

    uint64_t entry = elf_load(r, binary, size);
    if (!entry)
        return NULL;

    uint64_t ustack = uspace_build_stack(r);
    if (!ustack)
        return NULL;

    task_t *t = task_create_user(task_name ? task_name : r->name,
                                 r, entry, ustack, 32 * 1024);
    if (!t)
        return NULL;

    t->cpu_affinity = cpu_affinity;
    if (realm_add_task(r, t) != 0) {
        t->state = TASK_ZOMBIE;
        return NULL;
    }

    r->state = REALM_RUNNING;
    kprintf("[REALM] Exec realm #%u '%s'  task=%u  entry=0x%llx  stack=0x%llx\n",
            r->id, r->name, t->pid,
            (unsigned long long)entry, (unsigned long long)ustack);
    return t;
}

// ── realm_remove_task ─────────────────────────────────────────────────────
void realm_remove_task(realm_t *r, struct task *t) {
    if (!r || !t) return;
    uint64_t rflags = spinlock_irq_acquire(&realm_lock);
    for (uint32_t i = 0; i < r->task_count; i++) {
        if (r->tasks[i] == (task_t *)t) {
            // Shift remaining tasks down
            for (uint32_t j = i; j < r->task_count - 1; j++)
                r->tasks[j] = r->tasks[j + 1];
            r->tasks[--r->task_count] = NULL;
            break;
        }
    }
    ((task_t *)t)->realm = NULL;
    ((task_t *)t)->page_table = NULL;
    if (r->task_count == 0 && r->state == REALM_RUNNING)
        r->state = REALM_STOPPED;
    spinlock_irq_release(&realm_lock, rflags);
}

// ── libos_init ────────────────────────────────────────────────────────────
// Phase 4: Create the LibOS realm as a placeholder.
// It gets CAP_LIBOS_MAP but has no modules loaded yet.
void libos_init(void) {
    libos_realm = realm_create(REALM_NATIVE, "LibOS");
    if (!libos_realm) {
        kprintf("[LIBOS] WARNING: Could not create LibOS realm\n");
        return;
    }
    libos_realm->caps |= CAP_LIBOS_MAP;
    libos_realm->state = REALM_RUNNING;

    // Create /libos/ directory in VFS
    vfs_mkdir("/libos", VFS_MODE_DIR);
    vfs_mkdir("/libos/native", VFS_MODE_DIR);
    vfs_mkdir("/libos/wasm", VFS_MODE_DIR);
    vfs_mkdir("/libos/linux", VFS_MODE_DIR);
    vfs_mkdir("/libos/win32", VFS_MODE_DIR);

    kprintf("[LIBOS] LibOS realm #%u created  (CAP_LIBOS_MAP granted)\n",
            libos_realm->id);
}
