#pragma once
#include <stdint.h>
#include <stddef.h>

#define VMM_FLAG_PRESENT  (1ULL<<0)
#define VMM_FLAG_WRITE    (1ULL<<1)
#define VMM_FLAG_USER     (1ULL<<2)
#define VMM_FLAG_PWT      (1ULL<<3)
#define VMM_FLAG_PCD      (1ULL<<4)
#define VMM_FLAG_ACCESSED (1ULL<<5)
#define VMM_FLAG_DIRTY    (1ULL<<6)
#define VMM_FLAG_HUGE     (1ULL<<7)
#define VMM_FLAG_GLOBAL   (1ULL<<8)
#define VMM_FLAG_NX       (1ULL<<63)

#define VMM_KERNEL_RW  (VMM_FLAG_PRESENT|VMM_FLAG_WRITE|VMM_FLAG_GLOBAL)
#define VMM_KERNEL_RO  (VMM_FLAG_PRESENT|VMM_FLAG_GLOBAL|VMM_FLAG_NX)
#define VMM_USER_RX    (VMM_FLAG_PRESENT|VMM_FLAG_USER)                    // read+exec, NX clear
#define VMM_USER_RW    (VMM_FLAG_PRESENT|VMM_FLAG_WRITE|VMM_FLAG_USER|VMM_FLAG_NX) // read+write, NX set (W^X)
#define VMM_USER_RO    (VMM_FLAG_PRESENT|VMM_FLAG_USER|VMM_FLAG_NX)        // read-only, NX set

typedef struct { uint64_t pml4_phys; } page_table_t;

void          vmm_init(void);
page_table_t *vmm_new_space(void);
int           vmm_map_page(page_table_t *pt, uint64_t virt, uint64_t phys, uint64_t flags);
void          vmm_unmap_page(page_table_t *pt, uint64_t virt);
uint64_t      vmm_virt_to_phys(page_table_t *pt, uint64_t virt);
void          vmm_load(page_table_t *pt);
page_table_t *vmm_current(void);

extern page_table_t *kernel_page_table;
