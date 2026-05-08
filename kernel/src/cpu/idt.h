#pragma once
#include <stdint.h>

#define IDT_GATE_INT   0xE
#define IDT_GATE_TRAP  0xF
#define IDT_DPL_KERNEL 0x00
#define IDT_DPL_USER   0x60
#define IDT_IST_NONE   0

typedef void (*isr_handler_fn)(void);

void idt_set_gate(uint8_t vector, isr_handler_fn handler,
                  uint8_t gate_type, uint8_t dpl, uint8_t ist);
void idt_init(void);    // BSP: build + load
void idt_reload(void);  // APs: just lidt (table already built by BSP)
