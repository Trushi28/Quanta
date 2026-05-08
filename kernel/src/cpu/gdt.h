#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  cpu/gdt.h — Global Descriptor Table
//  Layout: null / kcode / kdata / ucode / udata / TSS (2 slots)
// ---------------------------------------------------------------------------

#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18   // DPL=3
#define GDT_USER_DATA    0x20   // DPL=3
#define GDT_TSS_SEL      0x28

// Initialise and load GDT + TSS on the calling CPU.
// Safe to call on both BSP and APs.
void gdt_init(void);
