#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  acpi/acpi.h — ACPI table parsing (RSDP, XSDT/RSDT, MADT, HPET)
// ---------------------------------------------------------------------------

// ── RSDP ──────────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    char     signature[8];     // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;         // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;     // physical (ACPI 1.0)
    // Extended fields (revision >= 2)
    uint32_t length;
    uint64_t xsdt_address;     // physical (ACPI 2.0+)
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

// ── Generic System Description Table header ───────────────────────────────
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

// ── XSDT / RSDT ───────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t          tables[];   // array of 64-bit physical addresses
} acpi_xsdt_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t          tables[];   // array of 32-bit physical addresses
} acpi_rsdt_t;

// ── MADT (Multiple APIC Description Table, "APIC") ────────────────────────
#define ACPI_MADT_SIG  "APIC"

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t          lapic_phys;    // local APIC physical address
    uint32_t          flags;         // bit 0: dual 8259s present
    uint8_t           entries[];
} acpi_madt_t;

// MADT entry types
#define MADT_TYPE_LAPIC         0
#define MADT_TYPE_IOAPIC        1
#define MADT_TYPE_ISO           2   // Interrupt Source Override
#define MADT_TYPE_NMI           3
#define MADT_TYPE_LAPIC_NMI     4
#define MADT_TYPE_LAPIC_ADDR    5   // 64-bit LAPIC address override
#define MADT_TYPE_X2APIC        9

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  length;
} madt_entry_hdr_t;

typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint8_t  acpi_uid;
    uint8_t  apic_id;
    uint32_t flags;    // bit 0: enabled, bit 1: online-capable
} madt_lapic_t;

typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;    // global system interrupt base
} madt_ioapic_t;

typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint8_t  bus_source;  // always 0 (ISA)
    uint8_t  irq_source;  // ISA IRQ
    uint32_t gsi;         // global system interrupt (IOAPIC input)
    uint16_t flags;       // polarity, trigger mode
} madt_iso_t;

typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint8_t  x2apic_id_lo;
    uint8_t  reserved[3];
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_uid;
} madt_x2apic_t;

// ── HPET table ────────────────────────────────────────────────────────────
#define ACPI_HPET_SIG  "HPET"
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t  hardware_id;
    uint8_t   addr_space_id;    // 0 = memory
    uint8_t   bit_width;
    uint8_t   bit_offset;
    uint8_t   access_size;
    uint64_t  base_address;
    uint8_t   hpet_number;
    uint16_t  min_tick;         // minimum clock ticks between interrupts
    uint8_t   protection;
} acpi_hpet_t;

// ── MCFG table (PCIe ECAM) ────────────────────────────────────────────────
#define ACPI_MCFG_SIG  "MCFG"
typedef struct __attribute__((packed)) {
    uint64_t base_address;
    uint16_t pci_segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} mcfg_entry_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t reserved;
    mcfg_entry_t entries[];
} acpi_mcfg_t;

// ── Public API ────────────────────────────────────────────────────────────

// Initialise ACPI from the RSDP virtual address provided by Limine.
void acpi_init(void *rsdp_virt);

// Find a table by its 4-character signature. Returns virtual pointer or NULL.
acpi_sdt_header_t *acpi_find_table(const char sig[4]);

// Accessors for specific tables (NULL if not found).
acpi_madt_t  *acpi_madt(void);
acpi_hpet_t  *acpi_hpet(void);
acpi_mcfg_t  *acpi_mcfg(void);

// Iterate over MADT entries of a specific type.
// Calls cb(entry, userdata) for each matching entry. Stops if cb returns 0.
typedef int (*madt_iter_fn)(const madt_entry_hdr_t *, void *);
void acpi_madt_foreach(uint8_t type, madt_iter_fn cb, void *ud);

// Physical → virtual conversion (set by PMM init, used for ACPI MMIO)
void *acpi_phys_to_virt(uint64_t phys);
