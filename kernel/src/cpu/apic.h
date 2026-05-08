#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  cpu/apic.h — Local APIC driver (supports both xAPIC and x2APIC modes)
//
//  The APIC is the modern interrupt controller on every x86 core.
//  x2APIC mode uses MSR writes instead of MMIO — faster and required on
//  systems with > 255 CPUs.
// ---------------------------------------------------------------------------

// Spurious vector — must have bits 3:0 all set (conventionally 0xFF)
#define APIC_SPURIOUS_VECTOR  0xFF
#define APIC_TIMER_VECTOR     0x30   // vector 48
#define APIC_IPI_PANIC_VECTOR 0xFE
#define APIC_IPI_SCHED_VECTOR 0x31   // vector 49 — TLB shootdown / reschedule

// LVT delivery modes
#define APIC_LVT_FIXED     0x000
#define APIC_LVT_SMI       0x200
#define APIC_LVT_NMI       0x400
#define APIC_LVT_EXTINT    0x700
#define APIC_LVT_MASKED    (1 << 16)

// Timer modes (bits 18:17 of LVT_TIMER)
#define APIC_TIMER_ONESHOT  (0 << 17)
#define APIC_TIMER_PERIODIC (1 << 17)
#define APIC_TIMER_TSCDEADLINE (2 << 17)

// ICR delivery modes (for sending IPIs)
#define APIC_ICR_FIXED    0x00000
#define APIC_ICR_LOWEST   0x00100
#define APIC_ICR_SMI      0x00200
#define APIC_ICR_NMI      0x00400
#define APIC_ICR_INIT     0x00500
#define APIC_ICR_SIPI     0x00600

#define APIC_ICR_PHYSICAL 0x000000   // dest mode: physical
#define APIC_ICR_LOGICAL  0x000800   // dest mode: logical

#define APIC_ICR_ASSERT   0x004000
#define APIC_ICR_LEVEL    0x008000

#define APIC_ICR_DEST_SELF    0x040000
#define APIC_ICR_DEST_ALL     0x080000
#define APIC_ICR_DEST_ALL_EXC 0x0C0000

// Initialise the local APIC for the current CPU.
// Must be called on every CPU (BSP + each AP).
void apic_init(void);

// Signal end of interrupt to the APIC.
void apic_eoi(void);

// Send an IPI to a specific LAPIC ID.
void apic_send_ipi(uint32_t lapic_id, uint8_t vector);

// Broadcast IPI to all-except-self.
void apic_send_ipi_all(uint8_t vector);

// Return the LAPIC ID of the current CPU.
uint32_t apic_id(void);

// Calibrate the APIC timer against TSC, then arm it to fire every
// `ms` milliseconds (periodic mode).
void apic_timer_init(uint32_t ms);

// Stop the APIC timer on this CPU.
void apic_timer_stop(void);

// True if running in x2APIC mode.
bool apic_x2apic_mode(void);
