#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  cpu/cpuid.h — CPUID instruction wrappers + feature queries
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} cpuid_regs_t;

static inline cpuid_regs_t cpuid(uint32_t leaf, uint32_t subleaf) {
    cpuid_regs_t r;
    __asm__ volatile ("cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf));
    return r;
}

// Feature bits — CPUID leaf 1
static inline bool cpu_has_apic(void)  { return (cpuid(1,0).edx >> 9) & 1; }
static inline bool cpu_has_x2apic(void){ return (cpuid(1,0).ecx >> 21) & 1; }
static inline bool cpu_has_tsc(void)   { return (cpuid(1,0).edx >> 4) & 1; }
static inline bool cpu_has_msr(void)   { return (cpuid(1,0).edx >> 5) & 1; }
static inline bool cpu_has_sse(void)   { return (cpuid(1,0).edx >> 25) & 1; }
static inline bool cpu_has_sse2(void)  { return (cpuid(1,0).edx >> 26) & 1; }

// CPUID leaf 0x80000001
static inline bool cpu_has_nx(void) {
    cpuid_regs_t r = cpuid(0x80000001, 0);
    return (r.edx >> 20) & 1;
}

// Invariant TSC (leaf 0x80000007, edx bit 8)
static inline bool cpu_has_invariant_tsc(void) {
    cpuid_regs_t r = cpuid(0x80000007, 0);
    return (r.edx >> 8) & 1;
}

// Read CPU vendor string into 13-byte buffer (12 chars + null terminator)
static inline void cpu_vendor(char out[13]) {
    cpuid_regs_t r = cpuid(0, 0);
    ((uint32_t *)out)[0] = r.ebx;
    ((uint32_t *)out)[1] = r.edx;
    ((uint32_t *)out)[2] = r.ecx;
    out[12] = '\0';
}

// Read CPU brand string into 49-byte buffer
static inline void cpu_brand(char out[49]) {
    for (int i = 0; i < 3; i++) {
        cpuid_regs_t r = cpuid(0x80000002 + i, 0);
        ((uint32_t *)(out + i * 16))[0] = r.eax;
        ((uint32_t *)(out + i * 16))[1] = r.ebx;
        ((uint32_t *)(out + i * 16))[2] = r.ecx;
        ((uint32_t *)(out + i * 16))[3] = r.edx;
    }
    out[48] = '\0';
}
