// ============================================================
//  realm/uspace.c — User address space builder (Phase 4)
//
//  Allocates physical pages via PMM, maps them into a Realm's
//  isolated address space via VMM, and builds user stacks.
// ============================================================
#include "uspace.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include <stddef.h>

// ── uspace_map_pages ──────────────────────────────────────────────────────
int uspace_map_pages(realm_t *r, uint64_t vaddr, size_t n, uint64_t flags) {
    if (!r || !r->page_table || n == 0) return -1;

    for (size_t i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) {
            kprintf("[USPACE] OOM mapping page %zu at 0x%llx\n",
                    i, (unsigned long long)(vaddr + i * PAGE_SIZE));
            // Unmap already-mapped pages on failure
            for (size_t j = 0; j < i; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                uint64_t pa = vmm_virt_to_phys(r->page_table, va);
                vmm_unmap_page(r->page_table, va);
                if (pa) pmm_free(pa & ~(PAGE_SIZE - 1));
            }
            return -1;
        }

        // Zero the page via HHDM
        memset(phys_to_virt(phys), 0, PAGE_SIZE);

        int rc = vmm_map_page(r->page_table, vaddr + i * PAGE_SIZE, phys, flags);
        if (rc != 0) {
            pmm_free(phys);
            // Unmap previously mapped
            for (size_t j = 0; j < i; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                uint64_t pa = vmm_virt_to_phys(r->page_table, va);
                vmm_unmap_page(r->page_table, va);
                if (pa) pmm_free(pa & ~(PAGE_SIZE - 1));
            }
            return -1;
        }

        r->page_count++;
    }

    return 0;
}

// ── uspace_unmap_pages ────────────────────────────────────────────────────
int uspace_unmap_pages(realm_t *r, uint64_t vaddr, size_t n) {
    if (!r || !r->page_table || n == 0) return -1;

    for (size_t i = 0; i < n; i++) {
        uint64_t va = vaddr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(r->page_table, va);
        vmm_unmap_page(r->page_table, va);
        if (phys) {
            pmm_free(phys & ~(PAGE_SIZE - 1));
            if (r->page_count > 0) r->page_count--;
        }
    }

    return 0;
}

// ── uspace_build_stack ────────────────────────────────────────────────────
// Allocates USER_STACK_PAGES and maps them at the top of user address space.
// Returns the initial user RSP (stack grows downward).
uint64_t uspace_build_stack(realm_t *r) {
    uint64_t stack_bottom = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);

    int rc = uspace_map_pages(r, stack_bottom, USER_STACK_PAGES, VMM_USER_RW);
    if (rc != 0) {
        kprintf("[USPACE] Failed to build user stack\n");
        return 0;
    }

    // Stack top = highest mapped address (stack grows down)
    // Leave 8 bytes at the very top for alignment
    uint64_t stack_top = USER_STACK_TOP - 8;

    kprintf("[USPACE] User stack: 0x%llx - 0x%llx  (%u pages)\n",
            (unsigned long long)stack_bottom,
            (unsigned long long)USER_STACK_TOP,
            USER_STACK_PAGES);

    return stack_top;
}
