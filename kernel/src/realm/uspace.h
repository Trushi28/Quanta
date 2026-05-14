#pragma once
#include <stdint.h>
#include <stddef.h>
#include "realm.h"

// ---------------------------------------------------------------------------
//  realm/uspace.h — User address space builder (Phase 4)
//
//  Provides page allocation/mapping primitives for Realm address spaces
//  and builds user stacks.
// ---------------------------------------------------------------------------

// User-space virtual address layout
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL   // just below canonical hole
#define USER_STACK_PAGES  16                       // 64 KiB user stack
#define USER_BASE         0x0000000000400000ULL    // default binary load address

// Map N fresh physical pages at vaddr in realm's address space
int uspace_map_pages(realm_t *r, uint64_t vaddr, size_t n, uint64_t flags);

// Unmap and free N pages from realm's address space
int uspace_unmap_pages(realm_t *r, uint64_t vaddr, size_t n);

// Build a user stack: allocate + map pages, return stack top vaddr
uint64_t uspace_build_stack(realm_t *r);
