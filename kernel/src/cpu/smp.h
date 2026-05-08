#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <limine.h>

// ---------------------------------------------------------------------------
//  cpu/smp.h — Symmetric Multi-Processing bringup
//
//  Quanta supports up to MAX_CPUS cores. Each CPU gets a cpu_local_t struct
//  accessible via the GS base MSR.
// ---------------------------------------------------------------------------

#define MAX_CPUS  64

// Per-CPU data block — must be the first thing in GS-addressable memory.
// Access it with: cpu_local()->field
typedef struct cpu_local {
    struct cpu_local *self;       // self-pointer (GS:0 == &cpu_local)
    uint32_t          cpu_id;     // logical CPU index (0 = BSP)
    uint32_t          lapic_id;   // hardware LAPIC ID
    uint64_t          ticks;      // per-CPU timer tick counter
    uint64_t          preempt_cnt;// preemption disable depth (0 = preemptible)
    void             *scheduler_sp; // saved RSP for the scheduler context
    void             *idle_stack; // per-CPU idle stack
    // Scratch space for syscall/interrupt handling
    uint64_t          kernel_stack_top;
    uint64_t          user_rsp_scratch;
} cpu_local_t;

// Initialise SMP: bring up all APs reported by Limine.
// Must be called after GDT, IDT, PMM, VMM, and APIC are initialised.
void smp_init(void);

// Must be called on the BSP before smp_init, after GDT is loaded.
void smp_bsp_early_init(void);

// Called on every AP entry point.
void ap_entry(struct limine_smp_info *info);

// Return the per-CPU block for the current processor.
static inline cpu_local_t *cpu_local(void) {
    cpu_local_t *p;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}

// Number of online CPUs (set after smp_init completes).
extern volatile uint32_t g_cpu_count;

// Array of per-CPU local blocks (index = logical CPU ID).
extern cpu_local_t g_cpu_locals[MAX_CPUS];

// Disable/enable preemption on this CPU.
static inline void preempt_disable(void) {
    cpu_local()->preempt_cnt++;
}
static inline void preempt_enable(void) {
    if (cpu_local()->preempt_cnt > 0)
        cpu_local()->preempt_cnt--;
}
static inline int preemptible(void) {
    return cpu_local()->preempt_cnt == 0;
}
