#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  cpu/gdt.h — Global Descriptor Table
//  Layout: null / kcode / kdata / udata / ucode / TSS (2 slots)
//
//  SYSRET requires user data BEFORE user code:
//    SYSRET SS = STAR[63:48] + 8  → 0x18 (udata)
//    SYSRET CS = STAR[63:48] + 16 → 0x20 (ucode)
// ---------------------------------------------------------------------------

// Raw segment offsets (no RPL bits)
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18   // DPL=3 — must precede ucode for SYSRET
#define GDT_USER_CODE    0x20   // DPL=3
#define GDT_TSS_SEL      0x28

// Selectors with RPL=3 for iretq frames and STAR MSR
#define GDT_USER_DATA_RPL3  (GDT_USER_DATA | 3)   // 0x1B
#define GDT_USER_CODE_RPL3  (GDT_USER_CODE | 3)   // 0x23

// Initialise and load GDT + TSS on the calling CPU.
// Safe to call on both BSP and APs.
void gdt_init(void);

// Update RSP0 in the current CPU's TSS. Must be called in sched_run_next()
// whenever switching to a user-mode task, so hardware interrupts land on
// the correct kernel stack.
void tss_set_rsp0(uint64_t rsp0);
