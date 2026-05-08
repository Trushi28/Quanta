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

void    *phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void *virt);
