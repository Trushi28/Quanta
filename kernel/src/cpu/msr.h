#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  cpu/msr.h — Model Specific Register helpers
// ---------------------------------------------------------------------------

// Well-known MSR addresses
#define MSR_APIC_BASE        0x0000001B
#define MSR_EFER             0xC0000080
#define MSR_STAR             0xC0000081
#define MSR_LSTAR            0xC0000082
#define MSR_CSTAR            0xC0000083
#define MSR_FMASK            0xC0000084
#define MSR_FS_BASE          0xC0000100
#define MSR_GS_BASE          0xC0000101
#define MSR_KERNEL_GS_BASE   0xC0000102
#define MSR_TSC_AUX          0xC0000103

// x2APIC MSRs (base 0x800, each register offset / 0x10 + 0x800)
#define MSR_X2APIC_ID        0x00000802
#define MSR_X2APIC_VER       0x00000803
#define MSR_X2APIC_TPR       0x00000808
#define MSR_X2APIC_EOI       0x0000080B
#define MSR_X2APIC_LDR       0x0000080D
#define MSR_X2APIC_SVR       0x0000080F
#define MSR_X2APIC_ISR0      0x00000810
#define MSR_X2APIC_IRR0      0x00000820
#define MSR_X2APIC_ESR       0x00000828
#define MSR_X2APIC_ICRL      0x00000830  // 64-bit write sends IPI
#define MSR_X2APIC_LVT_TIMER 0x00000832
#define MSR_X2APIC_LVT_LINT0 0x00000835
#define MSR_X2APIC_LVT_LINT1 0x00000836
#define MSR_X2APIC_LVT_ERROR 0x00000837
#define MSR_X2APIC_TIMER_ICR 0x00000838
#define MSR_X2APIC_TIMER_CCR 0x00000839
#define MSR_X2APIC_TIMER_DCR 0x0000083E

// APIC base MSR bits
#define APIC_BASE_BSP        (1ULL << 8)
#define APIC_BASE_X2APIC_EN  (1ULL << 10)
#define APIC_BASE_APIC_EN    (1ULL << 11)
#define APIC_BASE_ADDR_MASK  0x000FFFFF000ULL

// EFER bits
#define EFER_SCE  (1ULL << 0)   // Syscall Enable
#define EFER_LME  (1ULL << 8)   // Long Mode Enable
#define EFER_LMA  (1ULL << 10)  // Long Mode Active
#define EFER_NXE  (1ULL << 11)  // No-Execute Enable

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : :
        "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
