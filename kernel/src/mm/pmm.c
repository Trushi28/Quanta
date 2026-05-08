// ============================================================
//  mm/pmm.c — Bitmap physical memory manager
// ============================================================
#include "pmm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../lib/spinlock.h"
#include "../boot/limine_requests.h"
#include <stddef.h>

static uint8_t  *bitmap      = NULL;
static uint64_t  total_pages = 0;
static uint64_t  free_pages  = 0;
uint64_t         hhdm_off    = 0;   // also exported as hhdm_off_early
uint64_t         hhdm_off_early = 0; // set by kmain before pmm_init
static spinlock_t pmm_lock   = SPINLOCK_INIT;

// ── Bitmap primitives ─────────────────────────────────────────────────────
static inline void bm_set(uint64_t i)   { bitmap[i/8] |=  (uint8_t)(1u<<(i%8)); }
static inline void bm_clr(uint64_t i)   { bitmap[i/8] &= (uint8_t)~(1u<<(i%8)); }
static inline int  bm_tst(uint64_t i)   { return (bitmap[i/8]>>(i%8))&1; }

// ── Address conversion ────────────────────────────────────────────────────
void *phys_to_virt(uint64_t phys) {
    uint64_t off = hhdm_off ? hhdm_off : hhdm_off_early;
    return (void *)(phys + off);
}
uint64_t virt_to_phys(void *v) {
    uint64_t off = hhdm_off ? hhdm_off : hhdm_off_early;
    return (uint64_t)(uintptr_t)v - off;
}

// ── pmm_init ──────────────────────────────────────────────────────────────
void pmm_init(uint64_t hhdm_offset) {
    hhdm_off = hhdm_offset;
    hhdm_off_early = hhdm_offset;
    struct limine_memmap_response *mm = limine_memmap();
    if (!mm) kpanic("[PMM] No memory map\n");

    // Pass 1: find highest usable physical address
    uint64_t highest = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            uint64_t end = e->base + e->length;
            if (end > highest) highest = end;
        }
    }

    total_pages = highest / PAGE_SIZE;
    uint64_t bm_bytes = (total_pages + 7) / 8;

    // Pass 2: find region for bitmap
    uint64_t bm_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bm_bytes) {
            bm_phys = e->base;
            break;
        }
    }
    if (!bm_phys) kpanic("[PMM] No space for bitmap\n");

    bitmap = (uint8_t *)phys_to_virt(bm_phys);
    memset(bitmap, 0xFF, bm_bytes);   // all used
    free_pages = 0;

    // Pass 3: free all usable pages
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t base = PAGE_ALIGN_UP(e->base);
        uint64_t end  = PAGE_ALIGN_DOWN(e->base + e->length);
        for (uint64_t a = base; a < end; a += PAGE_SIZE) {
            uint64_t idx = a / PAGE_SIZE;
            if (idx < total_pages) { bm_clr(idx); free_pages++; }
        }
    }

    // Pass 4: re-mark bitmap pages as used
    uint64_t bm_pages = (bm_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t j = 0; j < bm_pages; j++) {
        uint64_t idx = (bm_phys / PAGE_SIZE) + j;
        if (!bm_tst(idx)) { bm_set(idx); free_pages--; }
    }

    kprintf("[PMM] bitmap @ phys 0x%llx  total=%llu pages  free=%llu MiB\n",
        (unsigned long long)bm_phys,
        (unsigned long long)total_pages,
        (unsigned long long)((free_pages * PAGE_SIZE) / (1024*1024)));
}

// ── pmm_alloc ─────────────────────────────────────────────────────────────
uint64_t pmm_alloc(void) {
    uint64_t rflags = spinlock_irq_acquire(&pmm_lock);
    for (uint64_t i = 1; i < total_pages; i++) {
        if (!bm_tst(i)) {
            bm_set(i); free_pages--;
            spinlock_irq_release(&pmm_lock, rflags);
            memset(phys_to_virt(i * PAGE_SIZE), 0, PAGE_SIZE);
            return i * PAGE_SIZE;
        }
    }
    spinlock_irq_release(&pmm_lock, rflags);
    return 0;
}

uint64_t pmm_alloc_n(size_t n) {
    if (!n) return 0;
    if (n == 1) return pmm_alloc();

    uint64_t rflags = spinlock_irq_acquire(&pmm_lock);
    uint64_t run_start = 0;
    size_t   run_len   = 0;
    for (uint64_t i = 1; i < total_pages; i++) {
        if (!bm_tst(i)) {
            if (!run_len) run_start = i;
            if (++run_len == n) {
                for (uint64_t j = run_start; j < run_start + n; j++) {
                    bm_set(j); free_pages--;
                }
                spinlock_irq_release(&pmm_lock, rflags);
                uint64_t phys = run_start * PAGE_SIZE;
                memset(phys_to_virt(phys), 0, n * PAGE_SIZE);
                return phys;
            }
        } else { run_len = 0; }
    }
    spinlock_irq_release(&pmm_lock, rflags);
    return 0;
}

void pmm_free(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE;
    if (!idx || idx >= total_pages) return;
    uint64_t rflags = spinlock_irq_acquire(&pmm_lock);
    if (bm_tst(idx)) { bm_clr(idx); free_pages++; }
    spinlock_irq_release(&pmm_lock, rflags);
}

void pmm_free_n(uint64_t phys, size_t n) {
    for (size_t i = 0; i < n; i++) pmm_free(phys + i * PAGE_SIZE);
}

void pmm_stats(void) {
    uint64_t used  = total_pages - free_pages;
    kprintf("[PMM] Total:%llu MiB  Used:%llu MiB  Free:%llu MiB\n",
        (unsigned long long)(total_pages*PAGE_SIZE/(1024*1024)),
        (unsigned long long)(used       *PAGE_SIZE/(1024*1024)),
        (unsigned long long)(free_pages *PAGE_SIZE/(1024*1024)));
}
