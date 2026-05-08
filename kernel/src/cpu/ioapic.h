#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  cpu/ioapic.h — I/O APIC driver
//
//  The I/O APIC is the system-level interrupt router.  External IRQs
//  (keyboard, timer, NIC...) are wired to its input pins.  Each pin has
//  a 64-bit "redirection entry" that maps it to a CPU vector + delivery
//  mode.  This replaces the legacy 8259 PIC → LINT0 path that is masked
//  by apic_init().
//
//  Address comes from the MADT (ACPI type-1 entry).
//  Registers are MMIO: write IOREGSEL, read/write IOWIN.
// ---------------------------------------------------------------------------

// Initialise the I/O APIC (finds address from MADT, masks all entries).
// Call after acpi_init() and pmm_init().
void ioapic_init(void);

// Redirect an ISA IRQ to a CPU vector on a specific LAPIC.
//   irq      – ISA IRQ number (0-23)
//   vector   – IDT vector to deliver (e.g. IRQ_BASE + irq  = 32+irq)
//   lapic_id – destination CPU (0 = BSP)
void ioapic_redirect(uint8_t irq, uint8_t vector, uint32_t lapic_id);

// Mask / unmask a single redirection entry.
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);

// True once ioapic_init() has succeeded.
int ioapic_available(void);
