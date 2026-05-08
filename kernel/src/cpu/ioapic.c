// ============================================================
//  cpu/ioapic.c — I/O APIC driver
//
//  Finds the IOAPIC via ACPI MADT, maps it through the HHDM,
//  and provides redirection-entry helpers so external IRQs
//  (keyboard, timer, ...) are delivered directly to CPU vectors
//  without going through the legacy 8259 PIC / LINT0 path.
// ============================================================
#include "ioapic.h"
#include "../acpi/acpi.h"
#include "../lib/kprintf.h"
#include "../mm/pmm.h"
#include <stddef.h>

// ── IOAPIC MMIO register indices ──────────────────────────────────────────
#define IOAPIC_REG_ID 0x00      // IOAPIC ID
#define IOAPIC_REG_VER 0x01     // version + max-redirection-entries
#define IOAPIC_REDTBL_BASE 0x10 // first redirection-entry lo register

// Register I/O: write selector to IOREGSEL (+0x00), data via IOWIN (+0x10)
#define IOREGSEL_OFF 0x00
#define IOWIN_OFF 0x10

// Redirection-entry bit fields (lo 32 bits)
#define IORED_VECTOR_MASK 0x000000FFu
#define IORED_DELIV_FIXED (0u << 8)   // Fixed delivery mode
#define IORED_DESTMODE_PHY (0u << 11) // Physical destination mode
#define IORED_POLARITY_HI (0u << 13)  // Active high
#define IORED_TRIGGER_EDGE (0u << 15) // Edge triggered
#define IORED_MASKED (1u << 16)       // Mask bit

// ── Module state ──────────────────────────────────────────────────────────
static uintptr_t g_base = 0;    // virtual base (HHDM-mapped)
static uint32_t g_gsi_base = 0; // global system interrupt base of this IOAPIC
static int g_ready = 0;

// ── Register helpers ──────────────────────────────────────────────────────
static uint32_t io_read(uint8_t reg) {
  *(volatile uint32_t *)(g_base + IOREGSEL_OFF) = reg;
  return *(volatile uint32_t *)(g_base + IOWIN_OFF);
}

static void io_write(uint8_t reg, uint32_t val) {
  *(volatile uint32_t *)(g_base + IOREGSEL_OFF) = reg;
  *(volatile uint32_t *)(g_base + IOWIN_OFF) = val;
}

// ── MADT callback: grab the first IOAPIC entry ───────────────────────────
static int ioapic_madt_cb(const madt_entry_hdr_t *e, void *ud) {
  (void)ud;
  const madt_ioapic_t *io = (const madt_ioapic_t *)e;
  if (g_base)
    return 0; // already have one, stop iterating
  g_base = (uintptr_t)phys_to_virt((uint64_t)io->ioapic_addr);
  g_gsi_base = io->gsi_base;
  return 0; // returning 0 stops the iterator after first match
}

// ── ioapic_init ────────────────────────────────────────────────────────────
void ioapic_init(void) {
  acpi_madt_foreach(MADT_TYPE_IOAPIC, ioapic_madt_cb, NULL);
  if (!g_base) {
    kprintf("[IOAPIC] Not found in MADT — keyboard will be unavailable\n");
    return;
  }

  uint32_t ver = io_read(IOAPIC_REG_VER);
  uint8_t max_redir = (uint8_t)((ver >> 16) & 0xFF);

  kprintf("[IOAPIC] virt=0x%lx  id=%u  max_redirs=%u  gsi_base=%u\n",
          (unsigned long)g_base, (io_read(IOAPIC_REG_ID) >> 24) & 0xF,
          max_redir + 1u, g_gsi_base);

  // Mask every entry at startup — individual drivers unmask what they need
  for (uint8_t i = 0; i <= max_redir; i++) {
    uint8_t lo_reg = (uint8_t)(IOAPIC_REDTBL_BASE + i * 2);
    uint8_t hi_reg = (uint8_t)(IOAPIC_REDTBL_BASE + i * 2 + 1);
    io_write(hi_reg, 0);
    io_write(lo_reg, IORED_MASKED | (uint32_t)(0x20 + i));
  }

  g_ready = 1;
}

// ── ioapic_redirect ────────────────────────────────────────────────────────
void ioapic_redirect(uint8_t irq, uint8_t vector, uint32_t lapic_id) {
  if (!g_ready)
    return;

  uint8_t pin = (uint8_t)(irq - g_gsi_base);
  uint8_t lo_reg = (uint8_t)(IOAPIC_REDTBL_BASE + pin * 2);
  uint8_t hi_reg = (uint8_t)(IOAPIC_REDTBL_BASE + pin * 2 + 1);

  uint32_t lo = (uint32_t)(vector & IORED_VECTOR_MASK) | IORED_DELIV_FIXED |
                IORED_DESTMODE_PHY | IORED_POLARITY_HI | IORED_TRIGGER_EDGE;
  // mask bit clear = unmasked

  uint32_t hi = lapic_id << 24;

  // Write high dword first, then low (avoids a brief unmasked misconfigured
  // state)
  io_write(hi_reg, hi);
  io_write(lo_reg, lo);
}

// ── ioapic_mask / ioapic_unmask ───────────────────────────────────────────
void ioapic_mask(uint8_t irq) {
  if (!g_ready)
    return;
  uint8_t lo_reg = (uint8_t)(IOAPIC_REDTBL_BASE + (irq - g_gsi_base) * 2);
  io_write(lo_reg, io_read(lo_reg) | IORED_MASKED);
}

void ioapic_unmask(uint8_t irq) {
  if (!g_ready)
    return;
  uint8_t lo_reg = (uint8_t)(IOAPIC_REDTBL_BASE + (irq - g_gsi_base) * 2);
  io_write(lo_reg, io_read(lo_reg) & ~IORED_MASKED);
}

int ioapic_available(void) { return g_ready; }
