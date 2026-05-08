#pragma once
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  mm/heap.h — Kernel heap (kmalloc / kfree / krealloc)
//
//  Algorithm: slab allocator for small sizes (8–2048 bytes) backed by
//  a free-list allocator for larger allocations.
//  Thread-safe via spinlock.
// ---------------------------------------------------------------------------

void  heap_init(void);

void *kmalloc (size_t size);
void *kcalloc (size_t n, size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree   (void *ptr);

// Debug: print heap statistics
void heap_stats(void);
