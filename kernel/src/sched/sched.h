#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../lib/list.h"
#include "../mm/vmm.h"

// ---------------------------------------------------------------------------
//  sched/sched.h — Preemptive round-robin scheduler
//
//  Phase 3 additions:
//    • sched_foreach_task() — iterate all tasks (used by `top` command)
//    • sched_get_tick()     — read the raw ms tick counter
//    • sched_task_count()   — number of live (non-zombie) tasks
//    • task_t gains cpu_id  — which CPU last ran this task
// ---------------------------------------------------------------------------

#define TASK_NAME_MAX  32

typedef enum {
    TASK_RUNNABLE = 0,
    TASK_RUNNING  = 1,
    TASK_SLEEPING = 2,
    TASK_BLOCKED  = 3,
    TASK_ZOMBIE   = 4,
} task_state_t;

typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t rip;
} task_ctx_t;

struct realm;  // forward declaration

typedef struct task {
    list_node_t   list;
    list_node_t   all_tasks;

    uint32_t      pid;
    uint32_t      cpu_affinity;   // 0xFF = any CPU
    uint32_t      last_cpu;       // which CPU last ran this task
    task_state_t  state;
    int           exit_code;

    task_ctx_t   *ctx;
    uint8_t      *stack;
    size_t        stack_size;

    uint64_t      ticks_total;    // total 1ms ticks consumed
    uint64_t      wake_tick;      // tick to wake up (if SLEEPING)

    // Phase 4: Realm association and user-mode context
    struct realm *realm;          // owning realm (NULL = kernel task)
    page_table_t *page_table;     // per-task page table (from realm)
    uint64_t      user_rip;       // Ring 3 entry point
    uint64_t      user_rsp;       // Ring 3 stack pointer

    char          name[TASK_NAME_MAX];
} task_t;

typedef void (*task_fn_t)(void *arg);

// ── Lifecycle ──────────────────────────────────────────────────────────────
void    sched_init(void);
task_t *task_create(const char *name, task_fn_t fn, void *arg, size_t stack_sz);
task_t *task_create_user(const char *name, struct realm *realm,
                         uint64_t entry, uint64_t user_stack,
                         size_t kernel_stack_sz);
void    sched_add(task_t *task);

// ── Scheduling ────────────────────────────────────────────────────────────
void     sched_tick(void);
void     sched_yield(void);
void     sched_sleep_ms(uint64_t ms);
__attribute__((noreturn)) void sched_exit(int code);

// ── Queries ───────────────────────────────────────────────────────────────
task_t  *sched_current(void);
uint64_t sched_uptime_ms(void);
uint64_t sched_get_tick(void);       // same as uptime_ms; alias for clarity
uint32_t sched_task_count(void);     // live (non-zombie) task count

// ── Iteration (holds sched lock for duration of callback) ─────────────────
//
//  IMPORTANT: do NOT call any scheduler functions (yield, sleep, etc.)
//  from inside the callback — the lock is already held.
//
typedef void (*task_iter_fn)(const task_t *t, void *ud);
void sched_foreach_task(task_iter_fn fn, void *ud);

// ── Context switch (in sched_asm.S) ──────────────────────────────────────
extern void sched_switch(task_ctx_t **old_ctx, task_ctx_t *new_ctx);
