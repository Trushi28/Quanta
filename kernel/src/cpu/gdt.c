// ============================================================
//  cpu/gdt.c — 64-bit GDT + TSS  (per-CPU)
// ============================================================
#include "gdt.h"
#include <stdint.h>
#include <stddef.h>

// ── Descriptor types ────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdtr_t;

// ── Per-CPU GDT storage ────────────────────────────────────────────────────
// MAX_CPUS copies of (5 normal + 2 TSS slot) = 7 entries each
#define MAX_CPUS_GDT 64
#define GDT_ENTRIES  7

static gdt_entry_t gdt_table[MAX_CPUS_GDT][GDT_ENTRIES];
static tss_t       tss_table[MAX_CPUS_GDT];

// Each CPU gets a 16 KiB kernel-entry stack for ring-0 transitions
#define CPU_IST_STACK_SIZE 4096
static uint8_t cpu_stacks[MAX_CPUS_GDT][CPU_IST_STACK_SIZE];

// ── Access / granularity flag macros ────────────────────────────────────────
#define ACCESS_PRESENT    0x80
#define ACCESS_RING0      0x00
#define ACCESS_RING3      0x60
#define ACCESS_CODE_DATA  0x10
#define ACCESS_CODE       0x0A
#define ACCESS_DATA       0x02
#define ACCESS_TSS        0x89

#define GRAN_4K       0x80
#define GRAN_64BIT    0x20
#define GRAN_LIMIT    0x0F

#define KCODE_ACCESS (ACCESS_PRESENT|ACCESS_RING0|ACCESS_CODE_DATA|ACCESS_CODE)
#define KDATA_ACCESS (ACCESS_PRESENT|ACCESS_RING0|ACCESS_CODE_DATA|ACCESS_DATA)
#define UCODE_ACCESS (ACCESS_PRESENT|ACCESS_RING3|ACCESS_CODE_DATA|ACCESS_CODE)
#define UDATA_ACCESS (ACCESS_PRESENT|ACCESS_RING3|ACCESS_CODE_DATA|ACCESS_DATA)

static gdt_entry_t make_entry(uint8_t access, uint8_t gran) {
    return (gdt_entry_t){
        .limit_low=0xFFFF,.base_low=0,.base_mid=0,
        .access=access,.granularity=gran,.base_high=0
    };
}

static void install_tss(gdt_entry_t table[GDT_ENTRIES],
                        tss_t *tss, uint8_t *stack) {
    tss->rsp0        = (uint64_t)(uintptr_t)(stack + CPU_IST_STACK_SIZE);
    tss->iopb_offset = sizeof(tss_t);

    gdt_tss_entry_t *e = (gdt_tss_entry_t *)&table[5];
    uintptr_t base  = (uintptr_t)tss;
    uint32_t  limit = sizeof(tss_t) - 1;
    e->limit_low  = (uint16_t)(limit & 0xFFFF);
    e->base_low   = (uint16_t)(base  & 0xFFFF);
    e->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    e->access     = ACCESS_TSS;
    e->granularity= (uint8_t)((limit >> 16) & 0x0F);
    e->base_high  = (uint8_t)((base >> 24) & 0xFF);
    e->base_upper = (uint32_t)(base >> 32);
    e->reserved   = 0;
}

// ── Assembly helpers ───────────────────────────────────────────────────────
__attribute__((naked)) void gdt_flush(uint64_t gdtr_ptr,
                                       uint16_t code_sel,
                                       uint16_t data_sel) {
    __asm__ volatile (
        "lgdt  (%rdi)          \n"
        "push  %rsi            \n"
        "lea   1f(%rip), %rax  \n"
        "push  %rax            \n"
        "lretq                 \n"
        "1:                    \n"
        "mov   %dx, %ds        \n"
        "mov   %dx, %es        \n"
        "mov   %dx, %fs        \n"
        "mov   %dx, %gs        \n"
        "mov   %dx, %ss        \n"
        "ret                   \n"
    );
}

__attribute__((naked)) void tss_flush(uint16_t tss_sel) {
    __asm__ volatile ("ltr %di\nret\n");
}

extern void gdt_flush(uint64_t, uint16_t, uint16_t);
extern void tss_flush(uint16_t);

// We need a cpu_id before GS is set up — use LAPIC ID from APIC MSR
// as a stable index. For simplicity we maintain a small atomic counter.
static volatile uint32_t gdt_init_counter = 0;

void gdt_init(void) {
    // Claim a slot (atomic fetch-add is safe even without GS)
    uint32_t slot = __atomic_fetch_add(&gdt_init_counter, 1, __ATOMIC_SEQ_CST);
    if (slot >= MAX_CPUS_GDT) slot = 0; // fallback to BSP slot

    gdt_entry_t *gdt = gdt_table[slot];
    tss_t       *tss = &tss_table[slot];
    uint8_t     *stk = cpu_stacks[slot];

    gdt[0] = (gdt_entry_t){0};
    gdt[1] = make_entry(KCODE_ACCESS, GRAN_4K|GRAN_64BIT|GRAN_LIMIT);
    gdt[2] = make_entry(KDATA_ACCESS, GRAN_4K|GRAN_LIMIT);
    gdt[3] = make_entry(UCODE_ACCESS, GRAN_4K|GRAN_64BIT|GRAN_LIMIT);
    gdt[4] = make_entry(UDATA_ACCESS, GRAN_4K|GRAN_LIMIT);
    install_tss(gdt, tss, stk);

    volatile gdtr_t gdtr = {
        .limit = (uint16_t)(sizeof(gdt_entry_t) * GDT_ENTRIES - 1),
        .base  = (uint64_t)(uintptr_t)gdt
    };

    gdt_flush((uint64_t)(uintptr_t)&gdtr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);
    tss_flush(GDT_TSS_SEL);
}
