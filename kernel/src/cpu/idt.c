// ============================================================
//  cpu/idt.c — Interrupt Descriptor Table (shared by all CPUs)
// ============================================================
#include "idt.h"
#include "isr.h"
#include "gdt.h"
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_gate_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

static idt_gate_t        idt[256];
static volatile idtr_t   g_idtr;

void idt_set_gate(uint8_t vector, isr_handler_fn handler,
                  uint8_t gate_type, uint8_t dpl, uint8_t ist) {
    uintptr_t addr = (uintptr_t)handler;
    idt[vector] = (idt_gate_t){
        .offset_low  = (uint16_t)(addr & 0xFFFF),
        .selector    = GDT_KERNEL_CODE,
        .ist         = ist & 0x07,
        .type_attr   = (uint8_t)(0x80 | (dpl & 0x60) | (gate_type & 0x0F)),
        .offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF),
        .offset_high = (uint32_t)(addr >> 32),
        .reserved    = 0
    };
}

void idt_init(void) {
    isr_install_all();   // remap PIC (disabled for APIC) + install all stubs

    g_idtr.limit = sizeof(idt) - 1;
    g_idtr.base  = (uint64_t)(uintptr_t)idt;
    __asm__ volatile ("lidt %0" : : "m"(g_idtr) : "memory");
}

// APs call this after idt_init() has already been called on the BSP
void idt_reload(void) {
    __asm__ volatile ("lidt %0" : : "m"(g_idtr) : "memory");
}
