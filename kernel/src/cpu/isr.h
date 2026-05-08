#pragma once
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8;
    uint64_t rbp,rdi,rsi,rdx,rcx,rbx,rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip,cs,rflags,rsp,ss;
} registers_t;

void isr_dispatch(registers_t *r);

typedef void (*irq_handler_t)(registers_t *r);
void irq_register_handler(uint8_t irq_num, irq_handler_t handler);
void isr_install_all(void);

void pic_send_eoi  (uint8_t irq);
void pic_mask_irq  (uint8_t irq);
void pic_unmask_irq(uint8_t irq);
void pic_disable   (void);   // fully mask 8259 (use when IOAPIC takes over)

#define IRQ_BASE 32
