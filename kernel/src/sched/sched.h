#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../lib/list.h"

// ---------------------------------------------------------------------------
//  sched/sched.h — Preemptive round-robin scheduler
//
//  Each CPU runs its own run queue. The APIC timer fires every 1 ms
//  and calls sched_tick(). Tasks can voluntarily yield via sched_yield().
// ---------------------------------------------------------------------------

#define TASK_NAME_MAX  32

// Task states
typedef enum {
    TASK_RUNNABLE = 0,
    TASK_RUNNING  = 1,
    TASK_SLEEPING = 2,   // waiting for a wakeup
    TASK_BLOCKED  = 3,   // waiting for I/O
    TASK_ZOMBIE   = 4,   // exited, not yet reaped
} task_state_t;

// Saved register context for context switch
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t rip;   // return address (pushed by call to sched_switch)
} task_ctx_t;

typedef struct task {
    list_node_t   list;
    list_node_t   all_tasks;       // global task list

    uint32_t      pid;
    uint32_t      cpu_affinity;    // 0xFF = any CPU
    task_state_t  state;
    int           exit_code;

    task_ctx_t   *ctx;             // saved context (on the task's stack)
    uint8_t      *stack;           // kernel stack base
    size_t        stack_size;

    uint64_t      ticks_total;     // total ticks consumed
    uint64_t      wake_tick;       // tick to wake up (if SLEEPING)

    char          name[TASK_NAME_MAX];
} task_t;

typedef void (*task_fn_t)(void *arg);

// Initialise the scheduler (creates the idle task).
void sched_init(void);

// Create a new kernel task. Returns NULL on OOM.
task_t *task_create(const char *name, task_fn_t fn, void *arg, size_t stack_sz);

// Make a task runnable (called after task_create, or to wake it).
void sched_add(task_t *task);

// Called from the APIC timer IRQ handler — may switch tasks.
void sched_tick(void);

// Voluntarily surrender this CPU's timeslice.
void sched_yield(void);

// Put the current task to sleep for `ms` milliseconds.
void sched_sleep_ms(uint64_t ms);

// Exit the current task (does not return).
__attribute__((noreturn)) void sched_exit(int code);

// Return a pointer to the currently running task on this CPU.
task_t *sched_current(void);

// Total kernel uptime in milliseconds.
uint64_t sched_uptime_ms(void);

// Low-level context switch (in sched_asm.S)
extern void sched_switch(task_ctx_t **old_ctx, task_ctx_t *new_ctx);
