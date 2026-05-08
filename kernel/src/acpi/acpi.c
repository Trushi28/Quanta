// ============================================================
//  acpi/acpi.c — ACPI table parsing
// ============================================================
#include "acpi.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include <stdint.h>

static acpi_xsdt_t  *g_xsdt     = NULL;
static acpi_rsdt_t  *g_rsdt     = NULL;
static int           g_use_xsdt = 0;

void *acpi_phys_to_virt(uint64_t phys) {
    return phys_to_virt(phys);
}

static uint8_t acpi_checksum(const void *data, size_t len) {
    const uint8_t *p = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += p[i];
    return sum;
}

void acpi_init(void *rsdp_virt) {
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)rsdp_virt;

    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        kpanic("[ACPI] Invalid RSDP signature\n");
    }
    if (acpi_checksum(rsdp, 20) != 0) {
        kpanic("[ACPI] RSDP checksum failed\n");
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        g_xsdt     = (acpi_xsdt_t *)phys_to_virt(rsdp->xsdt_address);
        g_use_xsdt = 1;
        kprintf("[ACPI] XSDT at phys 0x%llx  (%u bytes)\n",
                (unsigned long long)rsdp->xsdt_address,
                g_xsdt->header.length);
    } else {
        g_rsdt = (acpi_rsdt_t *)phys_to_virt((uint64_t)rsdp->rsdt_address);
        kprintf("[ACPI] RSDT at phys 0x%x  (%u bytes)\n",
                rsdp->rsdt_address, g_rsdt->header.length);
    }
}

acpi_sdt_header_t *acpi_find_table(const char sig[4]) {
    if (g_use_xsdt && g_xsdt) {
        uint64_t count = (g_xsdt->header.length - sizeof(acpi_sdt_header_t))
                         / sizeof(uint64_t);
        for (uint64_t i = 0; i < count; i++) {
            acpi_sdt_header_t *h =
                (acpi_sdt_header_t *)phys_to_virt(g_xsdt->tables[i]);
            if (memcmp(h->signature, sig, 4) == 0)
                return h;
        }
    } else if (g_rsdt) {
        uint32_t count = (g_rsdt->header.length - sizeof(acpi_sdt_header_t))
                         / sizeof(uint32_t);
        for (uint32_t i = 0; i < count; i++) {
            acpi_sdt_header_t *h =
                (acpi_sdt_header_t *)phys_to_virt((uint64_t)g_rsdt->tables[i]);
            if (memcmp(h->signature, sig, 4) == 0)
                return h;
        }
    }
    return NULL;
}

acpi_madt_t *acpi_madt(void) {
    return (acpi_madt_t *)acpi_find_table(ACPI_MADT_SIG);
}
acpi_hpet_t *acpi_hpet(void) {
    return (acpi_hpet_t *)acpi_find_table(ACPI_HPET_SIG);
}
acpi_mcfg_t *acpi_mcfg(void) {
    return (acpi_mcfg_t *)acpi_find_table(ACPI_MCFG_SIG);
}

void acpi_madt_foreach(uint8_t type, madt_iter_fn cb, void *ud) {
    acpi_madt_t *madt = acpi_madt();
    if (!madt) return;

    uint8_t *p   = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (p < end) {
        madt_entry_hdr_t *e = (madt_entry_hdr_t *)p;
        if (e->length < 2) break;
        if (e->type == type) {
            if (!cb(e, ud)) break;
        }
        p += e->length;
    }
}
