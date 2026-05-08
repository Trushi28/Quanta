// ============================================================
//  sched/sched.c — Preemptive round-robin scheduler
//
//  v2.1 fixes:
//   • sched_run_next keeps interrupts disabled across the
//     spinlock-release → sched_switch gap to prevent the
//     APIC timer from corrupting the scheduler state mid-switch.
//   • task_trampoline explicitly re-enables interrupts on
//     first entry (entered with IRQs disabled by the above).
// ============================================================
#include "sched.h"
#include "../cpu/smp.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include <stddef.h>

#define DEFAULT_STACK_SIZE (32 * 1024) // 32 KiB

// ── Global state ──────────────────────────────────────────────────────────
static spinlock_t sched_lock = SPINLOCK_INIT;
static list_t run_queue = LIST_INIT(run_queue);
static list_t all_tasks = LIST_INIT(all_tasks);
static uint32_t next_pid = 1;
static uint64_t g_tick = 0;

static task_t *cpu_current[MAX_CPUS];

// ── task_trampoline ───────────────────────────────────────────────────────
// Every new task enters here on its first scheduled run.
// At entry: rbx = fn, rbp = arg (set by setup_initial_stack).
// Interrupts are DISABLED here (held off since the context switch
// in sched_run_next kept them disabled through the switch).
// We re-enable them before calling the task function.
static void task_trampoline(void) {
  task_fn_t fn;
  void *arg;
  __asm__ volatile("mov %%rbx,%0\n mov %%rbp,%1" : "=r"(fn), "=r"(arg));

  // Safe to enable interrupts now — we are fully in the new task context.
  __asm__ volatile("sti");

  fn(arg);
  sched_exit(0);
}

// ── setup_initial_stack ───────────────────────────────────────────────────
// Builds the initial register frame so that sched_switch's pop sequence
// leaves rbx=fn, rbp=arg, and returns into task_trampoline.
//
// sched_switch pops in order: r15, r14, r13, r12, rbx, rbp  then ret.
// Stack layout from RSP (lowest address) upward:
//   [RSP+ 0] r15 = 0
//   [RSP+ 8] r14 = 0
//   [RSP+16] r13 = 0
//   [RSP+24] r12 = 0
//   [RSP+32] rbx = fn
//   [RSP+40] rbp = arg
//   [RSP+48] rip = task_trampoline
static void setup_initial_stack(task_t *t, task_fn_t fn, void *arg) {
  uint64_t *sp = (uint64_t *)(t->stack + t->stack_size);

  *--sp = (uint64_t)(uintptr_t)task_trampoline; // ret address
  *--sp = (uint64_t)(uintptr_t)arg;             // rbp
  *--sp = (uint64_t)(uintptr_t)fn;              // rbx
  *--sp = 0;                                    // r12
  *--sp = 0;                                    // r13
  *--sp = 0;                                    // r14
  *--sp = 0;                                    // r15

  t->ctx = (task_ctx_t *)sp;
}

// ── task_create ───────────────────────────────────────────────────────────
task_t *task_create(const char *name, task_fn_t fn, void *arg,
                    size_t stack_sz) {
  if (!stack_sz)
    stack_sz = DEFAULT_STACK_SIZE;
  stack_sz = PAGE_ALIGN_UP(stack_sz);

  task_t *t = (task_t *)kmalloc(sizeof(task_t));
  if (!t)
    return NULL;
  memset(t, 0, sizeof(*t));

  t->stack = (uint8_t *)kmalloc(stack_sz);
  if (!t->stack) {
    kfree(t);
    return NULL;
  }
  memset(t->stack, 0, stack_sz);

  t->stack_size = stack_sz;
  t->state = TASK_RUNNABLE;
  t->cpu_affinity = 0xFF;
  t->exit_code = 0;
  t->ticks_total = 0;

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
  if (!t)
    return;
  t->state = TASK_RUNNABLE;
  uint64_t rflags = spinlock_irq_acquire(&sched_lock);
  list_append(&run_queue, &t->list);
  spinlock_irq_release(&sched_lock, rflags);
}

// ── Idle task ─────────────────────────────────────────────────────────────
static void idle_fn(void *arg) {
  (void)arg;
  for (;;)
    __asm__ volatile("hlt");
}

// ── sched_init ────────────────────────────────────────────────────────────
void sched_init(void) {
  task_t *idle = task_create("idle/0", idle_fn, NULL, 8192);
  if (!idle)
    kpanic("[SCHED] Cannot create idle task\n");
  idle->state = TASK_RUNNING;
  cpu_current[0] = idle;
  kprintf("[SCHED] Scheduler initialised  pid=1 idle task ready\n");
}

task_t *sched_current(void) {
  uint32_t id = cpu_local()->cpu_id;
  return cpu_current[id < MAX_CPUS ? id : 0];
}

uint64_t sched_uptime_ms(void) { return g_tick; }

// ── sched_run_next ────────────────────────────────────────────────────────
// THE CRITICAL FIX:
//   We acquire the lock with interrupt-save (CLI + spinlock).
//   We RELEASE the spinlock alone (spinlock_release, NOT irq_release)
//   before sched_switch, keeping interrupts DISABLED through the switch.
//   This closes the race window where an APIC timer interrupt could fire
//   between "cpu_current updated" and "sched_switch executed", which
//   would corrupt the incoming task's initial stack context.
//
//   When a RESUMED task returns from sched_switch, it re-enables
//   interrupts itself (using the saved rflags).
//   When a NEW task starts for the first time, task_trampoline
//   calls sti before invoking the task function.
static void sched_run_next(void) {
  uint32_t cpu_id = cpu_local()->cpu_id;
  task_t *cur = cpu_current[cpu_id];

  // Acquire lock AND disable interrupts atomically.
  uint64_t rflags = spinlock_irq_acquire(&sched_lock);

  // Re-queue current task if it is still runnable.
  if (cur && cur->state == TASK_RUNNING) {
    cur->state = TASK_RUNNABLE;
    list_append(&run_queue, &cur->list);
  }

  // Pick next task.
  list_node_t *next_node = list_pop_front(&run_queue);
  if (!next_node) {
    // Nothing to run — stay on current task.
    if (cur)
      cur->state = TASK_RUNNING;
    spinlock_irq_release(&sched_lock, rflags);
    return;
  }

  task_t *next = container_of(next_node, task_t, list);
  next->state = TASK_RUNNING;
  cpu_current[cpu_id] = next;

  // Release the spinlock WITHOUT restoring interrupts.
  // Interrupts remain disabled until after sched_switch completes.
  spinlock_release(&sched_lock);

  if (cur != next) {
    sched_switch(&cur->ctx, next->ctx);
    // We arrive here when THIS task is switched back in.
    // Interrupts are still disabled — restore them now.
  }

  // Re-enable interrupts if they were enabled when we yielded.
  if (rflags & (1ULL << 9))
    __asm__ volatile("sti");
}

// ── sched_tick ────────────────────────────────────────────────────────────
void sched_tick(void) {
  __atomic_fetch_add(&g_tick, 1, __ATOMIC_RELAXED);
  cpu_local()->ticks++;

  // Wake sleeping tasks.
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

void sched_yield(void) { sched_run_next(); }

void sched_sleep_ms(uint64_t ms) {
  task_t *cur = sched_current();
  if (!cur)
    return;
  uint64_t rflags = spinlock_irq_acquire(&sched_lock);
  cur->wake_tick = g_tick + ms;
  cur->state = TASK_SLEEPING;
  spinlock_irq_release(&sched_lock, rflags);
  sched_yield();
}

__attribute__((noreturn)) void sched_exit(int code) {
  task_t *cur = sched_current();
  uint64_t rflags = spinlock_irq_acquire(&sched_lock);
  cur->state = TASK_ZOMBIE;
  cur->exit_code = code;
  spinlock_irq_release(&sched_lock, rflags);
  sched_yield();
  for (;;)
    __asm__ volatile("hlt");
}
