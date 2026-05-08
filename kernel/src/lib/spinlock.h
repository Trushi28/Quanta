#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  lib/spinlock.h — Ticket spinlock (fair, SMP-safe)
//
//  Uses a ticket algorithm to ensure FIFO ordering among waiters.
//  Interrupts should be disabled by the caller if the lock could be
//  acquired from an interrupt handler (use spinlock_irq_acquire).
// ---------------------------------------------------------------------------

typedef struct {
    volatile uint32_t ticket;   // next ticket to issue
    volatile uint32_t serving;  // ticket currently being served
} spinlock_t;

#define SPINLOCK_INIT  { .ticket = 0, .serving = 0 }

static inline void spinlock_init(spinlock_t *l) {
    l->ticket  = 0;
    l->serving = 0;
}

static inline void spinlock_acquire(spinlock_t *l) {
    uint32_t my_ticket = __atomic_fetch_add(&l->ticket, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != my_ticket)
        __asm__ volatile ("pause");
}

static inline void spinlock_release(spinlock_t *l) {
    __atomic_fetch_add(&l->serving, 1, __ATOMIC_RELEASE);
}

static inline bool spinlock_try_acquire(spinlock_t *l) {
    uint32_t serving = __atomic_load_n(&l->serving, __ATOMIC_RELAXED);
    uint32_t ticket  = serving;
    return __atomic_compare_exchange_n(&l->ticket, &ticket, serving + 1,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

// RFLAGS-saving variants for use in ISRs
static inline uint64_t spinlock_irq_acquire(spinlock_t *l) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));
    spinlock_acquire(l);
    return rflags;
}
static inline void spinlock_irq_release(spinlock_t *l, uint64_t rflags) {
    spinlock_release(l);
    if (rflags & (1 << 9)) __asm__ volatile ("sti");
}
