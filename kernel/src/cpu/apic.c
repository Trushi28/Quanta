// ============================================================
//  cpu/apic.c — Local APIC driver (xAPIC + x2APIC)
//
//  Phase 3 change: replaced the spin-loop calibration with a
//  PIT channel-2 hardware measurement.  The old loop was CPU-
//  speed dependent and produced wildly wrong tick rates on fast
//  or slow hosts, causing sched_sleep_ms() to sleep for the
//  wrong duration.  The PIT runs at a fixed 1.193182 MHz so the
//  measurement is always accurate regardless of clock speed.
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
static uint64_t g_xapic_base = 0;

// ── Low-level LAPIC read/write ────────────────────────────────────────────
static uint32_t lapic_read(uint32_t reg) {
    if (g_x2apic)
        return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return *(volatile uint32_t *)(g_xapic_base + reg);
}

static void lapic_write(uint32_t reg, uint32_t val) {
    if (g_x2apic)
        wrmsr(0x800 + (reg >> 4), val);
    else
        *(volatile uint32_t *)(g_xapic_base + reg) = val;
}

// ── x86 port I/O (needed for PIT calibration) ────────────────────────────
static inline uint8_t pit_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void pit_outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port) : "memory");
}

// ── PIT-based APIC timer calibration ─────────────────────────────────────
//
//  Uses 8254 PIT channel 2 (connected to the keyboard controller
//  gate port 0x61) to measure exactly CAL_MS milliseconds.
//
//  PIT base frequency: 1193182 Hz (±50 ppm — far more accurate
//  than a software spin loop).
//
//  DCR is set to divide-by-1 (0xB) here and must be set the same
//  way in apic_timer_init() so the tick rate stays consistent.
//
#define PIT_FREQ_HZ   1193182UL
#define CAL_MS        10
#define PIT_TICKS_CAL ((PIT_FREQ_HZ * CAL_MS) / 1000)   // = 11931

#define PIT_CH2_PORT  0x42
#define PIT_CMD_PORT  0x43
#define KBC_PORT      0x61

static uint64_t calibrate_with_pit(void) {
    uint8_t kbc = pit_inb(KBC_PORT);

    // Step 1: gate channel 2 off so we can reset it cleanly
    pit_outb(KBC_PORT, kbc & ~0x01u);

    // Step 2: program ch2 – mode 0 (terminal count), binary, lo+hi byte
    //   0xB0 = 1011 0000 = ch2, lobyte+hibyte, mode 0, binary
    pit_outb(PIT_CMD_PORT, 0xB0u);
    pit_outb(PIT_CH2_PORT, (uint8_t)(PIT_TICKS_CAL & 0xFF));
    pit_outb(PIT_CH2_PORT, (uint8_t)((PIT_TICKS_CAL >> 8) & 0xFF));

    // Step 3: reset APIC timer – divide-by-1, count from 0xFFFFFFFF
    lapic_write(XAPIC_TIMER_DCR, 0xBu);        // divide by 1
    lapic_write(XAPIC_TIMER_ICR, 0xFFFFFFFFu); // start counting down

    // Step 4: enable ch2 gate (bit 0 = 1); keep speaker muted (bit 1 = 0)
    pit_outb(KBC_PORT, (kbc & ~0x02u) | 0x01u);

    // Step 5: spin until OUT2 (bit 5 of port 0x61) goes high = count done
    while (!(pit_inb(KBC_PORT) & 0x20u))
        __asm__ volatile ("pause");

    // Step 6: sample current APIC count and stop the timer
    uint32_t remain = lapic_read(XAPIC_TIMER_CCR);
    lapic_write(XAPIC_TIMER_ICR, 0); // stop

    // Step 7: restore keyboard controller port
    pit_outb(KBC_PORT, kbc);

    uint32_t elapsed = 0xFFFFFFFFu - remain;

    // ticks_per_ms = elapsed_ticks_in_10ms / 10
    uint64_t tpm = (uint64_t)elapsed / CAL_MS;

    if (tpm == 0) {
        kprintf("[APIC] WARNING: PIT calibration returned 0, using fallback\n");
        tpm = 1000; // safe fallback (~1 GHz bus / 1 MHz timer)
    }

    kprintf("[APIC] PIT calibration: %u APIC ticks in %u ms → %llu ticks/ms\n",
            (unsigned)elapsed, CAL_MS, (unsigned long long)tpm);
    return tpm;
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
        uint64_t icr = ((uint64_t)lapic_id << 32) |
                       APIC_ICR_ASSERT | APIC_ICR_PHYSICAL | vector;
        wrmsr(0x830, icr);
    } else {
        lapic_write(XAPIC_ICRHI, lapic_id << 24);
        lapic_write(XAPIC_ICRLO, APIC_ICR_ASSERT | APIC_ICR_PHYSICAL | vector);
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

    g_x2apic = cpu_has_x2apic() && (apic_base_msr & APIC_BASE_X2APIC_EN);

    if (!g_x2apic) {
        g_xapic_base = (uintptr_t)phys_to_virt(apic_base_msr & APIC_BASE_ADDR_MASK);
        apic_base_msr |= APIC_BASE_APIC_EN;
        wrmsr(MSR_APIC_BASE, apic_base_msr);
    }

    lapic_write(XAPIC_SVR, 0x100 | APIC_SPURIOUS_VECTOR);
    lapic_write(XAPIC_LVT_LINT0, APIC_LVT_MASKED);
    lapic_write(XAPIC_LVT_LINT1, APIC_LVT_MASKED);
    lapic_write(XAPIC_LVT_ERROR, APIC_LVT_MASKED);
    lapic_write(XAPIC_ESR, 0);
    lapic_write(XAPIC_TPR, 0);
    lapic_write(XAPIC_EOI, 0);
}

// ── APIC Timer ────────────────────────────────────────────────────────────

static uint64_t g_ticks_per_ms = 0;

void apic_timer_init(uint32_t period_ms) {
    if (g_ticks_per_ms == 0) {
        // First call: calibrate once using the PIT.
        // Subsequent CPUs reuse the same value (SMP: all cores share the
        // same bus clock, so the tick rate is the same on every CPU).
        g_ticks_per_ms = calibrate_with_pit();
    }

    // Divide-by-1 (DCR = 0xB) must match what calibrate_with_pit() used.
    lapic_write(XAPIC_TIMER_DCR, 0xBu);
    lapic_write(XAPIC_LVT_TIMER, APIC_TIMER_VECTOR | APIC_TIMER_PERIODIC);
    lapic_write(XAPIC_TIMER_ICR, (uint32_t)(g_ticks_per_ms * period_ms));
}

void apic_timer_stop(void) {
    lapic_write(XAPIC_LVT_TIMER, APIC_LVT_MASKED);
    lapic_write(XAPIC_TIMER_ICR, 0);
}

// Return calibrated ticks per millisecond (read-only for other subsystems)
uint64_t apic_ticks_per_ms(void) { return g_ticks_per_ms; }
