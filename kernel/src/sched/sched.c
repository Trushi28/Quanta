// ============================================================
//  sched/sched.c — Preemptive round-robin scheduler
// ============================================================
#include "sched.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../lib/spinlock.h"
#include "../cpu/smp.h"
#include <stddef.h>

#define DEFAULT_STACK_SIZE  (32 * 1024)  // 32 KiB

// ── Global state ──────────────────────────────────────────────────────────
static spinlock_t  sched_lock     = SPINLOCK_INIT;
static list_t      run_queue      = LIST_INIT(run_queue);
static list_t      all_tasks      = LIST_INIT(all_tasks);
static uint32_t    next_pid       = 1;
static uint64_t    g_tick         = 0;   // global tick counter (1 ms per tick)

// Per-CPU current task
static task_t *cpu_current[MAX_CPUS];

// ── Helper: initialise a new stack so sched_switch can enter the task ─────
// The initial stack layout must match what sched_switch expects to pop:
//   r15, r14, r13, r12, rbx, rbp, rip (= task_trampoline)
static void task_trampoline(void);  // forward

static void setup_initial_stack(task_t *t, task_fn_t fn, void *arg) {
    // Top of stack (high address)
    uint64_t *sp = (uint64_t *)(t->stack + t->stack_size);

    // Push sentinel return address (task_trampoline does sched_exit)
    *--sp = (uint64_t)(uintptr_t)task_trampoline;

    // The task entry point: we use a trampoline that calls fn(arg)
    // We store fn and arg just below the task_trampoline frame
    // by encoding them as rbp=arg, rbx=fn in the saved-reg area:
    *--sp = (uint64_t)(uintptr_t)arg;   // will be in rbp after pop
    *--sp = (uint64_t)(uintptr_t)fn;    // will be in rbx after pop

    // r15 .. r12 (we set r15 to a marker so we can identify first-entry)
    *--sp = 0;  // r15
    *--sp = 0;  // r14
    *--sp = 0;  // r13
    *--sp = 0;  // r12

    // At this point the layout from bottom-to-top is:
    // [r12][r13][r14][r15][rbx=fn][rbp=arg][rip=entry_trampoline]
    // But sched_switch pops in order: r15,r14,r13,r12,rbx,rbp,ret
    // So we need layout (RSP points here after push of all):
    //   +0:r15  +8:r14  +16:r13  +24:r12  +32:rbx  +40:rbp  +48:rip
    // Let's fix: rebuild properly

    // Reset and build correctly bottom-up
    sp = (uint64_t *)(t->stack + t->stack_size);

    // sched_switch pops: r15,r14,r13,r12,rbx,rbp  then ret
    // Stack grows down; first push = highest address
    *--sp = (uint64_t)(uintptr_t)task_trampoline;  // ret address (rip)
    *--sp = (uint64_t)(uintptr_t)arg;              // rbp = arg
    *--sp = (uint64_t)(uintptr_t)fn;               // rbx = fn
    *--sp = 0; // r12
    *--sp = 0; // r13
    *--sp = 0; // r14
    *--sp = 0; // r15

    t->ctx = (task_ctx_t *)sp;
}

// Trampoline: called by sched_switch on first entry to a task
static void task_trampoline(void) {
    // At entry: rbx = fn, rbp = arg  (set by setup_initial_stack)
    task_fn_t fn;
    void     *arg;
    __asm__ volatile ("mov %%rbx,%0\n mov %%rbp,%1":"=r"(fn),"=r"(arg));
    fn(arg);
    sched_exit(0);
}

// ── task_create ───────────────────────────────────────────────────────────
task_t *task_create(const char *name, task_fn_t fn, void *arg, size_t stack_sz) {
    if (!stack_sz) stack_sz = DEFAULT_STACK_SIZE;
    stack_sz = PAGE_ALIGN_UP(stack_sz);

    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    t->stack = (uint8_t *)kmalloc(stack_sz);
    if (!t->stack) { kfree(t); return NULL; }
    memset(t->stack, 0, stack_sz);

    t->stack_size    = stack_sz;
    t->state         = TASK_RUNNABLE;
    t->cpu_affinity  = 0xFF;
    t->exit_code     = 0;
    t->ticks_total   = 0;

    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    t->pid = next_pid++;
    spinlock_irq_release(&sched_lock, rflags);

    strncpy(t->name, name ? name : "unnamed", TASK_NAME_MAX - 1);
    list_init(&t->list);
    list_init(&t->all_tasks);

    setup_initial_stack(t, fn, arg);

    rflags = spinlock_irq_acquire(&sched_lock);
    list_append(&all_tasks, &t->all_tasks);
    spinlock_irq_release(&sched_lock, rflags);

    return t;
}

void sched_add(task_t *t) {
    if (!t) return;
    t->state = TASK_RUNNABLE;
    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    list_append(&run_queue, &t->list);
    spinlock_irq_release(&sched_lock, rflags);
}

// ── Idle task ─────────────────────────────────────────────────────────────
static void idle_fn(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile ("hlt");
}

// ── sched_init ────────────────────────────────────────────────────────────
void sched_init(void) {
    task_t *idle = task_create("idle/0", idle_fn, NULL, 8192);
    if (!idle) kpanic("[SCHED] Cannot create idle task\n");
    idle->state = TASK_RUNNING;
    cpu_current[0] = idle;
    kprintf("[SCHED] Scheduler initialised  pid=1 idle task ready\n");
}

// ── sched_current ─────────────────────────────────────────────────────────
task_t *sched_current(void) {
    uint32_t id = cpu_local()->cpu_id;
    return cpu_current[id < MAX_CPUS ? id : 0];
}

uint64_t sched_uptime_ms(void) { return g_tick; }

// ── Core scheduler: pick next runnable task ────────────────────────────────
static void sched_run_next(void) {
    uint32_t cpu_id = cpu_local()->cpu_id;
    task_t  *cur    = cpu_current[cpu_id];

    uint64_t rflags = spinlock_irq_acquire(&sched_lock);

    // Re-queue current task if it's still runnable
    if (cur && cur->state == TASK_RUNNING) {
        cur->state = TASK_RUNNABLE;
        list_append(&run_queue, &cur->list);
    }

    // Pick next
    list_node_t *next_node = list_pop_front(&run_queue);
    if (!next_node) {
        // No runnable task — just keep running the idle task
        if (cur) cur->state = TASK_RUNNING;
        spinlock_irq_release(&sched_lock, rflags);
        return;
    }

    task_t *next = container_of(next_node, task_t, list);
    next->state  = TASK_RUNNING;
    cpu_current[cpu_id] = next;

    spinlock_irq_release(&sched_lock, rflags);

    // Perform the actual context switch
    if (cur != next) {
        sched_switch(&cur->ctx, next->ctx);
    }
}

// ── sched_tick — called from APIC timer IRQ ────────────────────────────────
void sched_tick(void) {
    __atomic_fetch_add(&g_tick, 1, __ATOMIC_RELAXED);
    cpu_local()->ticks++;

    // Wake any sleeping tasks
    {
        uint64_t rflags = spinlock_irq_acquire(&sched_lock);
        list_foreach(&all_tasks, node) {
            task_t *t = container_of(node, task_t, all_tasks);
            if (t->state == TASK_SLEEPING && g_tick >= t->wake_tick) {
                t->state = TASK_RUNNABLE;
                list_append(&run_queue, &t->list);
            }
        }
        spinlock_irq_release(&sched_lock, rflags);
    }

    if (preemptible()) {
        sched_run_next();
    }
}

void sched_yield(void) {
    sched_run_next();
}

void sched_sleep_ms(uint64_t ms) {
    task_t *cur = sched_current();
    if (!cur) return;
    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    cur->wake_tick = g_tick + ms;
    cur->state     = TASK_SLEEPING;
    spinlock_irq_release(&sched_lock, rflags);
    sched_yield();
}

__attribute__((noreturn)) void sched_exit(int code) {
    task_t *cur = sched_current();
    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    cur->state     = TASK_ZOMBIE;
    cur->exit_code = code;
    spinlock_irq_release(&sched_lock, rflags);
    sched_yield();
    for (;;) __asm__ volatile ("hlt");  // should never be reached
}
