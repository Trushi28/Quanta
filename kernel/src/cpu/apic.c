// ============================================================
//  cpu/apic.c — Local APIC driver (xAPIC + x2APIC)
// ============================================================
#include "apic.h"
#include "msr.h"
#include "cpuid.h"
#include "../lib/kprintf.h"
#include "../mm/pmm.h"
#include <stddef.h>

// ── xAPIC MMIO register offsets (from LAPIC base) ─────────────────────────
#define XAPIC_ID        0x020
#define XAPIC_VER       0x030
#define XAPIC_TPR       0x080
#define XAPIC_EOI       0x0B0
#define XAPIC_SVR       0x0F0
#define XAPIC_ESR       0x280
#define XAPIC_ICRLO     0x300
#define XAPIC_ICRHI     0x310
#define XAPIC_LVT_TIMER 0x320
#define XAPIC_LVT_LINT0 0x350
#define XAPIC_LVT_LINT1 0x360
#define XAPIC_LVT_ERROR 0x370
#define XAPIC_TIMER_ICR 0x380
#define XAPIC_TIMER_CCR 0x390
#define XAPIC_TIMER_DCR 0x3E0

// ── Module state ──────────────────────────────────────────────────────────
static bool     g_x2apic     = false;
static uint64_t g_xapic_base = 0;   // virtual address of MMIO registers

// ── Low-level read/write ──────────────────────────────────────────────────

static uint32_t lapic_read(uint32_t reg) {
    if (g_x2apic) {
        // x2APIC: each MMIO register at offset reg maps to MSR (0x800 + reg/16)
        return (uint32_t)rdmsr(0x800 + (reg >> 4));
    } else {
        return *(volatile uint32_t *)(g_xapic_base + reg);
    }
}

static void lapic_write(uint32_t reg, uint32_t val) {
    if (g_x2apic) {
        wrmsr(0x800 + (reg >> 4), val);
    } else {
        *(volatile uint32_t *)(g_xapic_base + reg) = val;
    }
}

// ── Public API ────────────────────────────────────────────────────────────

bool apic_x2apic_mode(void) { return g_x2apic; }

uint32_t apic_id(void) {
    if (g_x2apic)
        return (uint32_t)rdmsr(MSR_X2APIC_ID);
    return lapic_read(XAPIC_ID) >> 24;
}

void apic_eoi(void) {
    lapic_write(XAPIC_EOI, 0);
}

void apic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    if (g_x2apic) {
        // x2APIC: single 64-bit write to ICR MSR
        uint64_t icr = ((uint64_t)lapic_id << 32) |
                       APIC_ICR_ASSERT | APIC_ICR_PHYSICAL | vector;
        wrmsr(0x830, icr);  // MSR_X2APIC_ICRL = 0x830
    } else {
        // xAPIC: write destination to ICRHI first, then ICRLO to fire
        lapic_write(XAPIC_ICRHI, lapic_id << 24);
        lapic_write(XAPIC_ICRLO, APIC_ICR_ASSERT | APIC_ICR_PHYSICAL | vector);
        // Wait for delivery
        while (lapic_read(XAPIC_ICRLO) & (1 << 12))
            __asm__ volatile ("pause");
    }
}

void apic_send_ipi_all(uint8_t vector) {
    if (g_x2apic) {
        wrmsr(0x830, (uint64_t)APIC_ICR_DEST_ALL_EXC | APIC_ICR_ASSERT | vector);
    } else {
        lapic_write(XAPIC_ICRHI, 0);
        lapic_write(XAPIC_ICRLO, APIC_ICR_DEST_ALL_EXC | APIC_ICR_ASSERT | vector);
        while (lapic_read(XAPIC_ICRLO) & (1 << 12))
            __asm__ volatile ("pause");
    }
}

void apic_init(void) {
    uint64_t apic_base_msr = rdmsr(MSR_APIC_BASE);

    // Check if Limine already enabled x2APIC (or if we can/should enable it)
    g_x2apic = cpu_has_x2apic() &&
               (apic_base_msr & APIC_BASE_X2APIC_EN);

    if (!g_x2apic) {
        // Enable in xAPIC mode
        g_xapic_base = (uintptr_t)phys_to_virt(apic_base_msr & APIC_BASE_ADDR_MASK);
        apic_base_msr |= APIC_BASE_APIC_EN;
        wrmsr(MSR_APIC_BASE, apic_base_msr);
    }
    // In x2APIC mode the APIC base MSR already has X2APIC_EN set by Limine

    // Set Spurious Vector Register: enable APIC + set spurious vector
    lapic_write(XAPIC_SVR, 0x100 | APIC_SPURIOUS_VECTOR);

    // Mask LINT0 and LINT1 (we use IOAPIC for external IRQs)
    lapic_write(XAPIC_LVT_LINT0, APIC_LVT_MASKED);
    lapic_write(XAPIC_LVT_LINT1, APIC_LVT_MASKED);

    // Mask error LVT initially; clear ESR
    lapic_write(XAPIC_LVT_ERROR, APIC_LVT_MASKED);
    lapic_write(XAPIC_ESR, 0);

    // Set task priority register to 0: accept all interrupts
    lapic_write(XAPIC_TPR, 0);

    // Issue a final EOI to clear any stale pending interrupt
    lapic_write(XAPIC_EOI, 0);
}

// ── APIC Timer ────────────────────────────────────────────────────────────
// We calibrate against a short TSC spin: count APIC ticks in ~10 ms,
// derive ticks-per-ms, then program periodic mode.

#define APIC_TIMER_CAL_MS  10

static uint64_t g_ticks_per_ms = 0;

void apic_timer_init(uint32_t period_ms) {
    if (g_ticks_per_ms == 0) {
        // Divider = 1 (DCR = 0xB)
        lapic_write(XAPIC_TIMER_DCR, 0xB);

        // Set initial count to max and count down for ~10 ms
        lapic_write(XAPIC_TIMER_ICR, 0xFFFFFFFF);

        // Busy-wait using TSC (assumes ~1 GHz+ CPU; good enough for calibration)
        // We'll refine this with HPET later; for now a simple iteration loop
        volatile uint64_t spin = 0;
        for (uint64_t i = 0; i < 1000000ULL * APIC_TIMER_CAL_MS; i++)
            spin++;
        (void)spin;

        uint32_t ticks_elapsed = 0xFFFFFFFF - lapic_read(XAPIC_TIMER_CCR);
        g_ticks_per_ms = ticks_elapsed / APIC_TIMER_CAL_MS;
        if (g_ticks_per_ms == 0) g_ticks_per_ms = 1000; // fallback

        // Stop the timer
        lapic_write(XAPIC_TIMER_ICR, 0);
    }

    // Program periodic timer
    lapic_write(XAPIC_TIMER_DCR, 0xB);  // divide by 1
    lapic_write(XAPIC_LVT_TIMER, APIC_TIMER_VECTOR | APIC_TIMER_PERIODIC);
    lapic_write(XAPIC_TIMER_ICR, (uint32_t)(g_ticks_per_ms * period_ms));
}

void apic_timer_stop(void) {
    lapic_write(XAPIC_LVT_TIMER, APIC_LVT_MASKED);
    lapic_write(XAPIC_TIMER_ICR, 0);
}
