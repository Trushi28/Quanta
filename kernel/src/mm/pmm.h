#pragma once
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE       4096ULL
#define PAGE_SHIFT      12
#define PAGE_ALIGN_UP(x)   (((uint64_t)(x)+PAGE_SIZE-1)&~(PAGE_SIZE-1))
#define PAGE_ALIGN_DOWN(x) ((uint64_t)(x)&~(PAGE_SIZE-1))

void     pmm_init(uint64_t hhdm_offset);
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_n(size_t n);
void     pmm_free(uint64_t phys);
void     pmm_free_n(uint64_t phys, size_t n);
void     pmm_stats(void);

// Phase 3: expose raw counters so the shell can draw a visual bar
void     pmm_get_stats(uint64_t *total_pages_out, uint64_t *free_pages_out);

void    *phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void *virt);
