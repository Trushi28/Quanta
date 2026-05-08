// ============================================================
//  mm/heap.c — Kernel slab + free-list allocator
//
//  Slabs: 8, 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
//  Large: direct PMM allocation with a header tag
// ============================================================
#include "heap.h"
#include "pmm.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include <stddef.h>
#include <stdint.h>

// ── Slab configuration ─────────────────────────────────────────────────────
#define NUM_SLABS    9
#define SLAB_SIZES   {8,16,32,64,128,256,512,1024,2048}
#define SLAB_MAGIC   0xA110C8A7  // "ALLOC8AT" mangled

static const size_t slab_sz[NUM_SLABS] = SLAB_SIZES;

// Free list node embedded in free chunks
typedef struct free_node { struct free_node *next; } free_node_t;

typedef struct {
    free_node_t *head;   // free list
    size_t       obj_sz; // object size
    uint64_t     allocs; // total allocations from this slab
    spinlock_t   lock;
} slab_cache_t;

static slab_cache_t slabs[NUM_SLABS];

// ── Large allocation header ────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t slab_idx;   // 0xFF = large allocation
    uint64_t size;       // bytes including this header (rounded to PAGE_SIZE)
} alloc_header_t;

#define HEADER_ALIGN  16   // alloc_header_t padded to 16 bytes
#define HEADER_SIZE   16

// ── Internal helpers ──────────────────────────────────────────────────────
static void slab_refill(slab_cache_t *sc) {
    // Allocate one page and carve it into objects
    uint64_t phys = pmm_alloc();
    if (!phys) kpanic("[HEAP] OOM in slab_refill\n");
    uint8_t *page = (uint8_t *)phys_to_virt(phys);
    size_t   n    = PAGE_SIZE / sc->obj_sz;
    for (size_t i = 0; i < n; i++) {
        free_node_t *node = (free_node_t *)(page + i * sc->obj_sz);
        node->next  = sc->head;
        sc->head    = node;
    }
}

static int slab_idx_for(size_t size) {
    for (int i = 0; i < NUM_SLABS; i++)
        if (size <= slab_sz[i]) return i;
    return -1;
}

// ── heap_init ─────────────────────────────────────────────────────────────
void heap_init(void) {
    for (int i = 0; i < NUM_SLABS; i++) {
        slabs[i].obj_sz = slab_sz[i];
        slabs[i].head   = NULL;
        slabs[i].allocs = 0;
        spinlock_init(&slabs[i].lock);
        slab_refill(&slabs[i]);   // pre-fill with one page each
    }
    kprintf("[HEAP] Slab allocator ready  (%d caches)\n", NUM_SLABS);
}

// ── kmalloc ───────────────────────────────────────────────────────────────
void *kmalloc(size_t size) {
    if (!size) return NULL;

    // Add header overhead and find slab
    size_t total = size + HEADER_SIZE;
    int    idx   = slab_idx_for(total);

    if (idx >= 0) {
        slab_cache_t *sc = &slabs[idx];
        uint64_t rflags = spinlock_irq_acquire(&sc->lock);
        if (!sc->head) slab_refill(sc);
        free_node_t *node = sc->head;
        sc->head = node->next;
        sc->allocs++;
        spinlock_irq_release(&sc->lock, rflags);

        alloc_header_t *hdr = (alloc_header_t *)node;
        hdr->magic    = SLAB_MAGIC;
        hdr->slab_idx = (uint32_t)idx;
        hdr->size     = slab_sz[idx];
        return (uint8_t *)hdr + HEADER_SIZE;
    }

    // Large allocation: use PMM directly
    size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_n(pages);
    if (!phys) return NULL;

    alloc_header_t *hdr = (alloc_header_t *)phys_to_virt(phys);
    hdr->magic    = SLAB_MAGIC;
    hdr->slab_idx = 0xFF;
    hdr->size     = pages * PAGE_SIZE;
    return (uint8_t *)hdr + HEADER_SIZE;
}

void *kcalloc(size_t n, size_t size) {
    void *p = kmalloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    alloc_header_t *hdr = (alloc_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (hdr->magic != SLAB_MAGIC) {
        kprintf("[HEAP] kfree: bad magic at %p — double-free or corruption?\n", ptr);
        return;
    }
    hdr->magic = 0;  // poison

    if (hdr->slab_idx == 0xFF) {
        // Large allocation
        size_t pages = hdr->size / PAGE_SIZE;
        pmm_free_n(virt_to_phys(hdr), pages);
        return;
    }

    slab_cache_t *sc = &slabs[hdr->slab_idx];
    free_node_t  *node = (free_node_t *)hdr;
    uint64_t rflags = spinlock_irq_acquire(&sc->lock);
    node->next = sc->head;
    sc->head   = node;
    spinlock_irq_release(&sc->lock, rflags);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return NULL; }

    alloc_header_t *hdr = (alloc_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    size_t old_data_sz = hdr->size - HEADER_SIZE;
    if (new_size <= old_data_sz) return ptr;  // fits in existing allocation

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_data_sz);
    kfree(ptr);
    return new_ptr;
}

void heap_stats(void) {
    kprintf("[HEAP] Slab usage:\n");
    for (int i = 0; i < NUM_SLABS; i++) {
        kprintf("  [%4zu bytes]  allocs=%llu\n",
                slab_sz[i], (unsigned long long)slabs[i].allocs);
    }
}
