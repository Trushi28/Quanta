// ============================================================
//  sched/sched.c — Preemptive round-robin scheduler
//
//  Phase 3 changes:
//    • sched_foreach_task() — expose all tasks to the shell `top`
//    • sched_task_count()   — live task count for status bar
//    • task_t.last_cpu      — track which CPU ran each task
//    • sched_sleep_ms(): set SLEEPING before yielding, never
//      re-queue the task if it was already woken by sched_tick
//      before the context switch completes (harmless but noisy)
//    • Idle tasks now store their cpu_id in last_cpu so `top`
//      can show them correctly
// ============================================================
#include "sched.h"
#include "../cpu/smp.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../realm/realm.h"
#include <stddef.h>

#define DEFAULT_STACK_SIZE (32 * 1024)

// ── Global state ──────────────────────────────────────────────────────────
static spinlock_t sched_lock = SPINLOCK_INIT;
static list_t     run_queue  = LIST_INIT(run_queue);
static list_t     all_tasks  = LIST_INIT(all_tasks);
static uint32_t   next_pid   = 1;
static volatile uint64_t g_tick = 0;

static task_t *cpu_current[MAX_CPUS];
static task_t *idle_tasks [MAX_CPUS];

static int task_can_run_on_cpu(const task_t *t, uint32_t cpu_id) {
    return t && (t->cpu_affinity == 0xFF || t->cpu_affinity == cpu_id);
}

static task_t *pop_next_runnable(uint32_t cpu_id) {
    list_foreach(&run_queue, node) {
        task_t *t = container_of(node, task_t, list);
        if (t->state != TASK_RUNNABLE) {
            list_remove(&t->list);
            continue;
        }
        if (task_can_run_on_cpu(t, cpu_id)) {
            list_remove(&t->list);
            return t;
        }
    }
    return NULL;
}

// ── task_trampoline ───────────────────────────────────────────────────────
static void task_trampoline(void) {
    task_fn_t fn;
    void *arg;
    __asm__ volatile ("mov %%rbx,%0\n mov %%rbp,%1" : "=r"(fn), "=r"(arg));
    __asm__ volatile ("sti");
    fn(arg);
    sched_exit(0);
}

// ── setup_initial_stack ───────────────────────────────────────────────────
static void setup_initial_stack(task_t *t, task_fn_t fn, void *arg) {
    uint64_t *sp = (uint64_t *)(t->stack + t->stack_size);
    *--sp = (uint64_t)(uintptr_t)task_trampoline;
    *--sp = (uint64_t)(uintptr_t)arg;   // rbp  (arg to trampoline)
    *--sp = (uint64_t)(uintptr_t)fn;    // rbx  (fn  to trampoline)
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; // r12..r15
    t->ctx = (task_ctx_t *)sp;
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

    t->stack_size   = stack_sz;
    t->state        = TASK_RUNNABLE;
    t->cpu_affinity = 0xFF;
    t->last_cpu     = 0xFF; // not yet scheduled

    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    t->pid = next_pid++;
    spinlock_irq_release(&sched_lock, rflags);

    strncpy(t->name, name ? name : "unnamed", TASK_NAME_MAX - 1);
    t->name[TASK_NAME_MAX - 1] = '\0';
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

// ── user_task_trampoline ──────────────────────────────────────────────────
// First function run by a user-mode task.  Builds an iretq frame on the
// kernel stack and drops to Ring 3.
static void user_task_trampoline(void) {
    task_t *cur = sched_current();

    // Set up KERNEL_GS_BASE for swapgs on syscall entry.
    // GS_BASE should be 0 (user GS) when we're in Ring 3.
    // KERNEL_GS_BASE should be the cpu_local_t pointer.
    cpu_local_t *cl = cpu_local();
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)cl);

    // Set kernel_stack_top in cpu_local for the syscall trampoline
    cl->kernel_stack_top = (uint64_t)(cur->stack + cur->stack_size);

    // Load the realm's page table
    if (cur->page_table)
        vmm_load(cur->page_table);

    // CRITICAL: Set GS_BASE to 0 (user value) BEFORE swapgs.
    // Setting the GS segment selector to 0 does NOT clear GS_BASE MSR.
    // We must use wrmsr to explicitly zero it so that after swapgs:
    //   GS_BASE       = KERNEL_GS_BASE (cpu_local) ← kernel will see this
    //   KERNEL_GS_BASE = old GS_BASE (0)           ← user value preserved
    // Wait — that's backwards. swapgs swaps GS_BASE ↔ KERNEL_GS_BASE.
    // We want after swapgs: GS_BASE=0 (user in Ring 3), KERNEL_GS_BASE=cpu_local
    // So BEFORE swapgs: GS_BASE must be cpu_local, KERNEL_GS_BASE must be 0.
    // Then swapgs gives: GS_BASE=0, KERNEL_GS_BASE=cpu_local. ✓
    // On syscall entry: swapgs again → GS_BASE=cpu_local. ✓
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    // GS_BASE is already cpu_local (we're in kernel mode).
    // After swapgs: GS_BASE=0, KERNEL_GS_BASE=cpu_local. Perfect.

    // Build iretq frame and jump to Ring 3:
    //   iretq pops: RIP, CS, RFLAGS, RSP, SS
    __asm__ volatile (
        "mov $0x1B, %%ax    \n"   // GDT_USER_DATA_RPL3 = 0x1B
        "mov %%ax, %%ds     \n"
        "mov %%ax, %%es     \n"
        "push $0x1B         \n"   // SS  = GDT_USER_DATA_RPL3
        "push %[ursp]       \n"   // RSP = user stack
        "push $0x202        \n"   // RFLAGS = IF set
        "push $0x23         \n"   // CS  = GDT_USER_CODE_RPL3
        "push %[urip]       \n"   // RIP = user entry point
        "swapgs             \n"   // GS_BASE=cpu_local → 0, KERNEL_GS_BASE=0 → cpu_local
        "iretq              \n"
        :
        : [ursp] "r"(cur->user_rsp),
          [urip] "r"(cur->user_rip)
        : "memory", "ax"
    );
    __builtin_unreachable();
}

// ── task_create_user ──────────────────────────────────────────────────────
task_t *task_create_user(const char *name, struct realm *realm,
                         uint64_t entry, uint64_t user_stack,
                         size_t kernel_stack_sz) {
    // Create a kernel task that will trampoline into Ring 3
    task_t *t = task_create(name, (task_fn_t)user_task_trampoline, NULL,
                            kernel_stack_sz ? kernel_stack_sz : 32 * 1024);
    if (!t) return NULL;

    t->realm      = realm;
    t->page_table = NULL;    // set by realm_add_task
    t->user_rip   = entry;
    t->user_rsp   = user_stack;

    // Pin to BSP until AP preempt_cnt bug is fixed
    t->cpu_affinity = 0;

    return t;
}

// ── Idle task ─────────────────────────────────────────────────────────────
static void idle_fn(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile ("hlt");
}

// ── sched_init ────────────────────────────────────────────────────────────
void sched_init(void) {
    for (int i = 0; i < MAX_CPUS; i++) {
        task_t *idle = task_create("idle", idle_fn, NULL, 8192);
        if (!idle) kpanic("[SCHED] Cannot create idle task\n");
        idle->state    = TASK_RUNNING;
        idle->last_cpu = (uint32_t)i;
        idle_tasks[i]  = idle;
        cpu_current[i] = idle;
    }
    kprintf("[SCHED] Scheduler initialised  %u idle tasks ready\n", MAX_CPUS);
}

// ── Queries ───────────────────────────────────────────────────────────────
task_t *sched_current(void) {
    uint32_t id = cpu_local()->cpu_id;
    return cpu_current[id < MAX_CPUS ? id : 0];
}

uint64_t sched_uptime_ms(void) {
    return __atomic_load_n(&g_tick, __ATOMIC_RELAXED);
}

uint64_t sched_get_tick(void) {
    return sched_uptime_ms();
}

uint32_t sched_task_count(void) {
    uint32_t count = 0;
    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    list_foreach(&all_tasks, node) {
        task_t *t = container_of(node, task_t, all_tasks);
        if (t->state != TASK_ZOMBIE) count++;
    }
    spinlock_irq_release(&sched_lock, rflags);
    return count;
}

// ── foreach_task ──────────────────────────────────────────────────────────
void sched_foreach_task(task_iter_fn fn, void *ud) {
    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    list_foreach(&all_tasks, node) {
        task_t *t = container_of(node, task_t, all_tasks);
        fn(t, ud);
    }
    spinlock_irq_release(&sched_lock, rflags);
}

// ── sched_run_next ────────────────────────────────────────────────────────
static void sched_run_next(void) {
    uint32_t cpu_id = cpu_local()->cpu_id;
    task_t *cur = cpu_current[cpu_id];

    uint64_t rflags = spinlock_irq_acquire(&sched_lock);

    // Re-queue current task only if it's still runnable (not sleeping/blocked)
    if (cur && cur->state == TASK_RUNNING && cur != idle_tasks[cpu_id]) {
        cur->state = TASK_RUNNABLE;
        list_append(&run_queue, &cur->list);
    }

    task_t *next = pop_next_runnable(cpu_id);
    if (!next)
        next = idle_tasks[cpu_id];

    next->state    = TASK_RUNNING;
    next->last_cpu = cpu_id;
    cpu_current[cpu_id] = next;

    // Phase 4: TSS RSP0 update for user-mode tasks
    // When a Ring 3 task takes a hardware interrupt, the CPU loads RSP0
    // from the TSS.  We must point it at this task's kernel stack.
    if (next->realm != NULL) {
        tss_set_rsp0((uint64_t)(next->stack + next->stack_size));
    }

    // Phase 4: Page table switching
    // User tasks have their own address space; kernel tasks share the
    // kernel page table.
    if (next->page_table) {
        vmm_load(next->page_table);
    } else {
        // Switching from user task back to kernel task
        vmm_load(kernel_page_table);
    }

    spinlock_release(&sched_lock);

    if (cur != next) {
        if (cur) {
            sched_switch(&cur->ctx, next->ctx);
        } else {
            task_ctx_t *dummy;
            sched_switch(&dummy, next->ctx);
        }
    }

    // Re-enable interrupts if they were on before we acquired the lock
    if (rflags & (1ULL << 9))
        __asm__ volatile ("sti");
}

// ── sched_tick ────────────────────────────────────────────────────────────
void sched_tick(void) {
    __atomic_fetch_add(&g_tick, 1, __ATOMIC_RELAXED);
    cpu_local()->ticks++;

    // Increment this CPU's current task tick counter
    uint32_t cpu_id = cpu_local()->cpu_id;
    if (cpu_id < MAX_CPUS && cpu_current[cpu_id])
        cpu_current[cpu_id]->ticks_total++;

    // Wake sleeping tasks whose deadline has passed
    {
        uint64_t tick = __atomic_load_n(&g_tick, __ATOMIC_RELAXED);
        uint64_t rflags = spinlock_irq_acquire(&sched_lock);
        list_foreach(&all_tasks, node) {
            task_t *t = container_of(node, task_t, all_tasks);
            if (t->state == TASK_SLEEPING && tick >= t->wake_tick) {
                t->state = TASK_RUNNABLE;
                list_append(&run_queue, &t->list);
            }
        }
        spinlock_irq_release(&sched_lock, rflags);
    }

    if (preemptible())
        sched_run_next();
}

void sched_yield(void) { sched_run_next(); }

void sched_sleep_ms(uint64_t ms) {
    task_t *cur = sched_current();
    if (!cur || ms == 0) return;

    uint64_t tick_now = __atomic_load_n(&g_tick, __ATOMIC_RELAXED);

    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    cur->wake_tick = tick_now + ms;
    cur->state     = TASK_SLEEPING;
    // Remove from run queue if present (shouldn't be, but defensive)
    list_remove(&cur->list);
    spinlock_irq_release(&sched_lock, rflags);

    // Yield to the next runnable task; sched_tick() will wake us
    sched_run_next();
}

__attribute__((noreturn)) void sched_exit(int code) {
    task_t *cur = sched_current();
    if (cur && cur->realm)
        realm_remove_task((realm_t *)cur->realm, cur);
    uint64_t rflags = spinlock_irq_acquire(&sched_lock);
    cur->state     = TASK_ZOMBIE;
    cur->exit_code = code;
    spinlock_irq_release(&sched_lock, rflags);
    sched_yield();
    for (;;) __asm__ volatile ("hlt");
}
