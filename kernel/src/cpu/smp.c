// ============================================================
//  cpu/smp.c — SMP AP bringup
// ============================================================
#include "smp.h"
#include "../boot/limine_requests.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "msr.h"
#include <stddef.h>

volatile uint32_t g_cpu_count = 0;
cpu_local_t g_cpu_locals[MAX_CPUS];

// Per-CPU idle stack size
#define CPU_STACK_SIZE (16 * 1024) // 16 KiB

// ── Set up GS base to point at this CPU's cpu_local_t ─────────────────────
static void cpu_local_setup(uint32_t cpu_id, uint32_t lapic_id) {
  cpu_local_t *cl = &g_cpu_locals[cpu_id];
  memset(cl, 0, sizeof(*cl));
  cl->self = cl;
  cl->cpu_id = cpu_id;
  cl->lapic_id = lapic_id;
  cl->ticks = 0;
  cl->preempt_cnt = 1; // start with preemption disabled until scheduler runs

  // Write both GS_BASE and KERNEL_GS_BASE (for swapgs)
  wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)cl);
  wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)cl);
}

// ── BSP early setup (called before smp_init) ─────────────────────────────
void smp_bsp_early_init(void) {
  struct limine_smp_response *smp = limine_smp();
  uint32_t bsp_lapic = smp ? smp->bsp_lapic_id : 0;
  cpu_local_setup(0, bsp_lapic);
  __atomic_fetch_add(&g_cpu_count, 1, __ATOMIC_SEQ_CST);
}

// ── AP entry point ────────────────────────────────────────────────────────
// Limine calls this on each AP with a pointer to its limine_smp_info.
// Each AP has its own 64 KiB stack already set up by Limine.
void ap_entry(struct limine_smp_info *info) {
  uint32_t cpu_id = (uint32_t)info->extra_argument;

  gdt_init();
  idt_reload();

  apic_init();

  cpu_local_setup(cpu_id, info->lapic_id);

  apic_timer_init(1);

  // Signal to BSP that this AP is online
  __atomic_fetch_add(&g_cpu_count, 1, __ATOMIC_SEQ_CST);

  kprintf("[SMP] CPU %u online  (LAPIC %u%s)\n", cpu_id, info->lapic_id,
          apic_x2apic_mode() ? "  x2APIC" : "");

  // Enable interrupts and idle
  __asm__ volatile("sti");
  for (;;)
    __asm__ volatile("hlt");
}

// ── smp_init ─────────────────────────────────────────────────────────────
void smp_init(void) {
  struct limine_smp_response *smp = limine_smp();
  if (!smp) {
    kprintf("[SMP] No SMP response — uniprocessor mode\n");
    return;
  }

  bool x2apic = (smp->flags & LIMINE_SMP_X2APIC) != 0;
  kprintf("[SMP] %llu CPU(s) detected  BSP-LAPIC=%u  %s\n",
          (unsigned long long)smp->cpu_count, smp->bsp_lapic_id,
          x2apic ? "x2APIC" : "xAPIC");

  for (uint64_t i = 0; i < smp->cpu_count; i++) {
    struct limine_smp_info *ci = smp->cpus[i];

    if (ci->lapic_id == smp->bsp_lapic_id)
      continue;

    uint32_t cpu_id =
        (uint32_t)(__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST));
    if (cpu_id >= MAX_CPUS)
      break;

    ci->extra_argument = cpu_id;

    __atomic_store_n(&ci->goto_address, ap_entry, __ATOMIC_SEQ_CST);

    uint64_t timeout = 100000000ULL;
    while (__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST) <= cpu_id) {
      if (--timeout == 0) {
        kprintf("[SMP] Warning: AP (LAPIC %u) timed out waiting for online\n",
                ci->lapic_id);
        break;
      }
      __asm__ volatile("pause");
    }
  }

  // Wait for all APs to come online (with a timeout)
  uint64_t expected = smp->cpu_count;
  for (uint64_t spin = 0; spin < 100000000ULL; spin++) {
    if (__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST) >= expected)
      break;
    __asm__ volatile("pause");
  }

  kprintf("[SMP] %u CPU(s) online\n",
          __atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST));
}
