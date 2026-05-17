// ============================================================
//  mm/vmm.c — x86-64 four-level paging
// ============================================================
#include "vmm.h"
#include "pmm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../cpu/msr.h"
#include <stddef.h>

#define ENTRIES   512
#define PAGE_MASK (~(PAGE_SIZE-1))

static inline uint64_t virt_idx(uint64_t v, int lvl) {
    return (v >> (12 + (lvl-1)*9)) & 0x1FF;
}
static inline uint64_t entry_phys(uint64_t e) {
    return e & 0x000FFFFFFFFFF000ULL;
}
static uint64_t *alloc_table(void) {
    uint64_t p = pmm_alloc();
    if (!p) kpanic("[VMM] OOM allocating page table\n");
    return (uint64_t *)phys_to_virt(p);
}

page_table_t *kernel_page_table = NULL;

#define MAX_SPACES 128
static page_table_t pt_pool[MAX_SPACES];
static int          pt_used = 0;

static page_table_t *alloc_pt_struct(void) {
    if (pt_used >= MAX_SPACES) kpanic("[VMM] address-space pool exhausted\n");
    return &pt_pool[pt_used++];
}

page_table_t *vmm_new_space(void) {
    page_table_t *pt  = alloc_pt_struct();
    uint64_t     *pml4 = alloc_table();
    pt->pml4_phys = virt_to_phys(pml4);
    if (kernel_page_table) {
        uint64_t *kpml4 = (uint64_t *)phys_to_virt(kernel_page_table->pml4_phys);
        for (int i = 256; i < 512; i++) pml4[i] = kpml4[i];
    }
    return pt;
}

static uint64_t free_user_tree(uint64_t table_phys, int level) {
    uint64_t *table = (uint64_t *)phys_to_virt(table_phys);
    uint64_t freed = 0;

    for (int i = 0; i < ENTRIES; i++) {
        uint64_t entry = table[i];
        if (!(entry & VMM_FLAG_PRESENT))
            continue;

        uint64_t phys = entry_phys(entry);
        table[i] = 0;

        if (level == 1 || (entry & VMM_FLAG_HUGE)) {
            pmm_free(phys);
            freed++;
        } else {
            freed += free_user_tree(phys, level - 1);
            pmm_free(phys);
            freed++;
        }
    }

    return freed;
}

uint64_t vmm_destroy_space(page_table_t *pt) {
    if (!pt || pt == kernel_page_table || !pt->pml4_phys)
        return 0;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(pt->pml4_phys);
    uint64_t freed = 0;

    for (int i = 0; i < 256; i++) {
        uint64_t entry = pml4[i];
        if (!(entry & VMM_FLAG_PRESENT))
            continue;

        uint64_t phys = entry_phys(entry);
        pml4[i] = 0;
        freed += free_user_tree(phys, 3);
        pmm_free(phys);
        freed++;
    }

    pmm_free(pt->pml4_phys);
    pt->pml4_phys = 0;
    return freed + 1;
}

int vmm_map_page(page_table_t *pt, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pt) return -1;
    uint64_t *tables[4];
    tables[0] = (uint64_t *)phys_to_virt(pt->pml4_phys);
    for (int level = 4; level > 1; level--) {
        uint64_t idx   = virt_idx(virt, level);
        uint64_t entry = tables[4-level][idx];
        if (!(entry & VMM_FLAG_PRESENT)) {
            uint64_t *t  = alloc_table();
            uint64_t  np = virt_to_phys(t);
            tables[4-level][idx] = np | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
            tables[4-level+1] = t;
        } else {
            tables[4-level+1] = (uint64_t *)phys_to_virt(entry_phys(entry));
        }
    }
    tables[3][virt_idx(virt,1)] = (phys & PAGE_MASK) | (flags & ~PAGE_MASK);
    __asm__ volatile ("invlpg (%0)"::"r"(virt):"memory");
    return 0;
}

void vmm_unmap_page(page_table_t *pt, uint64_t virt) {
    if (!pt) return;
    uint64_t *cur = (uint64_t *)phys_to_virt(pt->pml4_phys);
    for (int level = 4; level > 1; level--) {
        uint64_t e = cur[virt_idx(virt,level)];
        if (!(e & VMM_FLAG_PRESENT)) return;
        cur = (uint64_t *)phys_to_virt(entry_phys(e));
    }
    cur[virt_idx(virt,1)] = 0;
    __asm__ volatile ("invlpg (%0)"::"r"(virt):"memory");
}

uint64_t vmm_virt_to_phys(page_table_t *pt, uint64_t virt) {
    if (!pt) return 0;
    uint64_t *cur = (uint64_t *)phys_to_virt(pt->pml4_phys);
    for (int level = 4; level > 1; level--) {
        uint64_t e = cur[virt_idx(virt,level)];
        if (!(e & VMM_FLAG_PRESENT)) return 0;
        cur = (uint64_t *)phys_to_virt(entry_phys(e));
    }
    uint64_t leaf = cur[virt_idx(virt,1)];
    if (!(leaf & VMM_FLAG_PRESENT)) return 0;
    return entry_phys(leaf) | (virt & (PAGE_SIZE-1));
}

void vmm_load(page_table_t *pt) {
    if (pt) __asm__ volatile ("mov %0,%%cr3"::"r"(pt->pml4_phys):"memory");
}

page_table_t *vmm_current(void) { return kernel_page_table; }

void vmm_cpu_init(void) {
    // Enable NXE + SCE in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE | EFER_SCE;
    wrmsr(MSR_EFER, efer);
}

void vmm_init(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3,%0":"=r"(cr3));
    kernel_page_table = alloc_pt_struct();
    kernel_page_table->pml4_phys = cr3 & PAGE_MASK;

    vmm_cpu_init();

    kprintf("[VMM] PML4 phys=0x%llx  NX+SCE enabled\n",
            (unsigned long long)kernel_page_table->pml4_phys);
}
