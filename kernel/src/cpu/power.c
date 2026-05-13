// ============================================================
//  cpu/power.c — ACPI power management (reboot + shutdown)
//
//  Reads the ACPI FADT at boot to locate:
//    - RESET_REG  (Generic Address Structure at FADT+116)
//    - PM1a/b_CNT_BLK  (I/O ports for ACPI sleep)
//  Scans the DSDT AML for the _S5_ package to obtain the
//  correct SLP_TYPa / SLP_TYPb values for S5 (soft off).
//
//  All FADT fields are accessed via byte offsets rather than a
//  packed struct so we never risk alignment assumptions and
//  avoid adding a large typedef to the shared acpi.h.
// ============================================================
#include "power.h"
#include "../acpi/acpi.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Port I/O helpers ─────────────────────────────────────────────────────
static inline uint8_t _inb(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb  %1,%0" : "=a"(v) : "Nd"(p) : "memory");
  return v;
}
static inline void _outb(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p) : "memory");
}
static inline void _outw(uint16_t p, uint16_t v) {
  __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p) : "memory");
}

// ── FADT byte-offset accessors ───────────────────────────────────────────
static inline uint8_t fadt_u8(const uint8_t *f, uint32_t off) { return f[off]; }
static inline uint16_t fadt_u16(const uint8_t *f, uint32_t off) {
  return (uint16_t)(f[off] | ((uint16_t)f[off + 1] << 8));
}
static inline uint32_t fadt_u32(const uint8_t *f, uint32_t off) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; i--)
    v = (v << 8) | f[off + i];
  return v;
}
static inline uint64_t fadt_u64(const uint8_t *f, uint32_t off) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--)
    v = (v << 8) | f[off + i];
  return v;
}

// ── Module state (populated by power_init) ───────────────────────────────
static uint32_t g_pm1a_cnt = 0; // I/O port for PM1a control
static uint32_t g_pm1b_cnt = 0; // I/O port for PM1b control (may be 0)
static uint8_t g_slp_typ_a = 5; // S5 sleep type for PM1a (ACPI default = 5)
static uint8_t g_slp_typ_b = 5; // S5 sleep type for PM1b
static bool g_has_reset = false;
static uint8_t g_reset_space = 1; // 1 = I/O port
static uint64_t g_reset_addr = 0;
static uint8_t g_reset_val = 0;
static bool g_power_ready = false;

// ── Minimal AML _S5_ parser ──────────────────────────────────────────────
// Searches DSDT AML for the pattern:
//   NameOp ('_','S','5','_') PackageOp PkgLength NumElements [SLP_TYPa]
//   [SLP_TYPb]
// Returns true if found; writes sleep types into *ta and *tb.
static bool find_s5(const uint8_t *aml, uint32_t len, uint8_t *ta,
                    uint8_t *tb) {
  if (!aml || len < 12)
    return false;

  for (uint32_t i = 0; i + 8 < len; i++) {
    // NameOp = 0x08 followed by 4-byte name segment
    if (aml[i] != 0x08)
      continue;
    if (aml[i + 1] != '_')
      continue;
    if (aml[i + 2] != 'S')
      continue;
    if (aml[i + 3] != '5')
      continue;
    if (aml[i + 4] != '_')
      continue;
    // Next byte should be PackageOp (0x12)
    const uint8_t *p = aml + i + 5;
    if (i + 5 >= len)
      break;
    if (*p != 0x12)
      continue;
    p++;
    if (p - aml >= (int)len)
      break;

    // Parse PkgLength (variable-length: top 2 bits = number of extra bytes)
    uint8_t b0 = *p++;
    uint32_t extra = (b0 >> 6) & 0x03;
    p += extra; // skip extra length bytes
    if (p - aml >= (int)len)
      break;

    // NumElements (1 byte, typically 4 for _S5_)
    uint8_t nelems = *p++;
    if (nelems == 0 || nelems > 8)
      continue;
    if (p - aml >= (int)len)
      break;

    // First element → SLP_TYPa
    if (*p == 0x00) {
      *ta = 0;
      p++;
    } // ZeroOp
    else if (*p == 0x01) {
      *ta = 1;
      p++;
    } // OneOp
    else if (*p == 0x0A) {
      *ta = *(p + 1);
      p += 2;
    } // ByteConst
    else
      continue;
    if (p - aml >= (int)len) {
      *tb = *ta;
      return true;
    }

    // Second element → SLP_TYPb
    if (*p == 0x00) {
      *tb = 0;
    } else if (*p == 0x01) {
      *tb = 1;
    } else if (*p == 0x0A) {
      *tb = *(p + 1);
    } else {
      *tb = *ta;
    }

    return true;
  }
  return false;
}

// ── power_init ────────────────────────────────────────────────────────────
void power_init(void) {
  // Locate FADT (signature "FACP")
  acpi_sdt_header_t *hdr = acpi_find_table("FACP");
  if (!hdr) {
    kprintf("[POWER] FADT not found — using fallback reboot/shutdown\n");
    g_power_ready = true;
    return;
  }

  const uint8_t *f = (const uint8_t *)hdr;
  uint32_t len = hdr->length;

  // ── PM1 control block ports (needed for S5 shutdown) ─────────────────
  if (len > 68) {
    g_pm1a_cnt = fadt_u32(f, 64); // PM1a_CNT_BLK at FADT+64
    g_pm1b_cnt = fadt_u32(f, 68); // PM1b_CNT_BLK at FADT+68
  }

  // ── Reset register (GAS at FADT+116, reset value at FADT+128) ─────────
  // Only present when FADT length > 128 (ACPI 2.0+)
  if (len > 128) {
    g_reset_space = fadt_u8(f, 116); // address space ID
    // f+117..119 = bit width, bit offset, access size (unused here)
    g_reset_addr = fadt_u64(f, 120); // 64-bit address
    g_reset_val = fadt_u8(f, 128);   // reset value
    g_has_reset = (g_reset_addr != 0);
  }

  // ── Locate DSDT to find _S5_ sleep types ─────────────────────────────
  uint64_t dsdt_phys = (uint64_t)fadt_u32(f, 40); // DSDT at FADT+40
  // Prefer X_DSDT (64-bit DSDT pointer, at FADT+140) when present
  if (len > 148) {
    uint64_t xdsdt = fadt_u64(f, 140);
    if (xdsdt)
      dsdt_phys = xdsdt;
  }

  if (dsdt_phys) {
    acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)phys_to_virt(dsdt_phys);
    uint32_t dsdt_len = dsdt->length;
    if (dsdt_len > sizeof(acpi_sdt_header_t)) {
      const uint8_t *aml = (const uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
      uint32_t aml_len = dsdt_len - (uint32_t)sizeof(acpi_sdt_header_t);
      uint8_t ta = 5, tb = 5;
      if (find_s5(aml, aml_len, &ta, &tb)) {
        g_slp_typ_a = ta;
        g_slp_typ_b = tb;
        kprintf("[POWER] _S5_ found: SLP_TYPa=%u SLP_TYPb=%u\n", ta, tb);
      } else {
        kprintf("[POWER] _S5_ not found, using default SLP_TYP=5\n");
      }
    }
  }

  kprintf("[POWER] PM1a=0x%x  PM1b=0x%x  reset_reg=%s\n", g_pm1a_cnt,
          g_pm1b_cnt, g_has_reset ? "yes" : "no");

  g_power_ready = true;
}

// ── power_reboot ──────────────────────────────────────────────────────────
__attribute__((noreturn)) void power_reboot(void) {
  __asm__ volatile("cli");

  // ── Method 1: ACPI FADT reset register (I/O space only for simplicity) ─
  if (g_has_reset && g_reset_space == 1 /* I/O */ && g_reset_addr) {
    _outb((uint16_t)(g_reset_addr & 0xFFFF), g_reset_val);
    // Spin briefly to let hardware catch up
    for (volatile int i = 0; i < 2000000; i++)
      __asm__ volatile("pause");
  }

  // ── Method 2: PS/2 keyboard-controller reset pulse ────────────────────
  // Flush the KBC output buffer, then pulse the CPU reset line.
  {
    int timeout = 65536;
    while (--timeout > 0) {
      uint8_t st = _inb(0x64);
      if (st & 0x01) {
        _inb(0x60);
        continue;
      } // drain output buffer
      if (!(st & 0x02))
        break; // input buffer empty
    }
    _outb(0x64, 0xFE); // pulse RESET# line
  }
  for (volatile int i = 0; i < 2000000; i++)
    __asm__ volatile("pause");

  // ── Method 3: PCIe/PIIX "system reset" via port 0xCF9 ────────────────
  _outb(0xCF9, 0x02); // SYS_RST
  for (volatile int i = 0; i < 100000; i++)
    __asm__ volatile("pause");
  _outb(0xCF9, 0x06); // full reset
  for (volatile int i = 0; i < 2000000; i++)
    __asm__ volatile("pause");

  // ── Method 4: Triple fault (always works on x86; last resort) ────────
  __asm__ volatile(
      "lidt (%%rsp)\n" // load a zero-limit IDT so any fault → triple-fault
      "ud2\n"          // raise invalid opcode (guaranteed exception)
      ::
          : "memory");

  for (;;)
    __asm__ volatile("hlt");
}

// ── power_shutdown ────────────────────────────────────────────────────────
__attribute__((noreturn)) void power_shutdown(void) {
  __asm__ volatile("cli");

  // SLP_EN bit (bit 13) OR'd with SLP_TYP (bits 12:10)
#define SLP_EN (1u << 13)
#define SLP_VAL(t) (uint16_t)(((uint16_t)(t) << 10) | SLP_EN)

  // ── Method 1: ACPI S5 via PM1a control block ─────────────────────────
  if (g_pm1a_cnt) {
    _outw((uint16_t)g_pm1a_cnt, SLP_VAL(g_slp_typ_a));
    for (volatile int i = 0; i < 2000000; i++)
      __asm__ volatile("pause");
  }
  if (g_pm1b_cnt) {
    _outw((uint16_t)g_pm1b_cnt, SLP_VAL(g_slp_typ_b));
    for (volatile int i = 0; i < 2000000; i++)
      __asm__ volatile("pause");
  }

#undef SLP_EN
#undef SLP_VAL

  // ── Method 2: QEMU PIIX4 APM shutdown (port 0x0604, value 0x3400) ────
  // S5 with SLP_TYP=5: (5<<10)|0x2000 = 0x1400|0x2000 = 0x3400
  _outw(0x0604, 0x3400);
  for (volatile int i = 0; i < 2000000; i++)
    __asm__ volatile("pause");

  // ── Method 3: Old QEMU / Bochs legacy APM ────────────────────────────
  _outw(0xB004, 0x2000);
  for (volatile int i = 0; i < 2000000; i++)
    __asm__ volatile("pause");

  // ── Nothing worked — just halt with a message ─────────────────────────
  kprintf(
      "\n[POWER] Shutdown failed. This system may need a manual power-off.\n");
  for (;;)
    __asm__ volatile("hlt");
}
