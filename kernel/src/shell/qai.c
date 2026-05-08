// ============================================================
//  shell/qai.c — Quanta AI (QAI) built-in knowledge assistant
//
//  Keyword-trie matching over a curated knowledge base.
//  No network, no external model — answers are compiled in.
//  Each entry has a set of trigger keywords and a response.
//  The best-matching entry (most keyword hits) is chosen.
// ============================================================
#include "qai.h"
#include "shell.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../sched/sched.h"
#include "../mm/pmm.h"
#include "../cpu/cpuid.h"
#include "../boot/limine_requests.h"
#include <stddef.h>

// ── Knowledge base entry ──────────────────────────────────────────────────
typedef struct {
    const char *keywords[8];   // trigger keywords (NULL-terminated list)
    const char *response;      // answer text (can include ANSI codes)
} kb_entry_t;

// ── Helper: convert a char to lowercase ───────────────────────────────────
static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// ── Case-insensitive substring search ─────────────────────────────────────
static int ci_contains(const char *haystack, const char *needle) {
    size_t nl = strlen(needle);
    size_t hl = strlen(haystack);
    if (nl > hl) return 0;
    for (size_t i = 0; i <= hl - nl; i++) {
        int match = 1;
        for (size_t j = 0; j < nl; j++) {
            if (to_lower(haystack[i+j]) != to_lower(needle[j])) {
                match = 0; break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

// ── Knowledge base ────────────────────────────────────────────────────────
static const kb_entry_t kb[] = {

    { {"what","is","quanta",NULL},
      "\033[96mQuanta OS\033[0m is a 64-bit kernel written in C, built from scratch.\n"
      "It features:\n"
      "  • x2APIC interrupt controller (faster than legacy 8259)\n"
      "  • SMP — symmetric multiprocessing (up to 64 CPUs)\n"
      "  • Preemptive round-robin scheduler with per-CPU run queues\n"
      "  • Slab + free-list kernel heap (kmalloc/kfree)\n"
      "  • Four-level paging (PML4) with NX bit enforcement\n"
      "  • VirtIO block device driver (virtio-blk 1.1)\n"
      "  • Virtual File System (VFS) with ramfs and devfs\n"
      "  • QAI — this AI assistant, compiled into the kernel\n"
    },

    { {"apic","x2apic","interrupt controller",NULL},
      "\033[96mAPIC (Advanced Programmable Interrupt Controller)\033[0m\n"
      "  The APIC replaced the old 8259 PIC. Quanta uses \033[93mx2APIC mode\033[0m\n"
      "  when available. x2APIC accesses registers via MSR writes\n"
      "  instead of MMIO, which is faster and supports >255 CPUs.\n"
      "  Quanta requests x2APIC from Limine via the SMP request flags.\n"
      "  Each CPU has its own local APIC (LAPIC) and the system has\n"
      "  one or more I/O APICs for routing external interrupts.\n"
    },

    { {"smp","multiprocessor","multi-core","cpu","cores",NULL},
      "\033[96mSMP — Symmetric Multi-Processing\033[0m\n"
      "  Quanta brings up all CPUs reported by the Limine SMP response.\n"
      "  Each AP (application processor) gets its own:\n"
      "    • GDT + TSS\n"
      "    • APIC timer (1 ms tick)\n"
      "    • cpu_local_t struct (accessed via GS base MSR)\n"
      "    • Run queue entry in the scheduler\n"
      "  The BSP (boot strap processor) wakes APs by atomically writing\n"
      "  their goto_address field in the limine_smp_info struct.\n"
    },

    { {"scheduler","scheduling","preemption","context switch","task",NULL},
      "\033[96mQuanta Scheduler\033[0m\n"
      "  Algorithm: Preemptive round-robin with per-CPU run queues.\n"
      "  The APIC timer fires every 1 ms and calls sched_tick().\n"
      "  Context switch saves/restores 6 callee-saved registers + RIP\n"
      "  (implemented in sched_asm.S).\n"
      "  Tasks can also voluntarily yield via sched_yield() or sleep\n"
      "  via sched_sleep_ms().\n"
      "  Use: \033[93mtasks\033[0m command to list running tasks.\n"
    },

    { {"paging","virtual memory","vmm","page table","pml4","tlb",NULL},
      "\033[96mVirtual Memory Manager (VMM)\033[0m\n"
      "  Quanta uses x86-64 four-level paging:\n"
      "    PML4 (512 GB/entry) → PDPT → PD → PT → 4 KB page\n"
      "  The kernel is loaded at 0xFFFFFFFF80000000 (upper 2 GB).\n"
      "  All physical RAM is accessible via the HHDM offset at\n"
      "  0xFFFF800000000000 (provided by Limine).\n"
      "  NX (no-execute) and SCE (syscall enable) are set in EFER.\n"
      "  Each new address space copies kernel PML4 entries 256-511.\n"
    },

    { {"pmm","physical memory","bitmap","page","allocation",NULL},
      "\033[96mPhysical Memory Manager (PMM)\033[0m\n"
      "  Algorithm: flat bitmap, 1 bit per 4 KiB page.\n"
      "  pmm_alloc()   — allocate one zeroed 4 KiB page\n"
      "  pmm_alloc_n() — allocate N contiguous pages\n"
      "  pmm_free()    — return a page to the free pool\n"
      "  Thread-safe via ticket spinlock.\n"
      "  Use: \033[93mmem\033[0m command to see current usage.\n"
    },

    { {"heap","kmalloc","kfree","slab","allocator",NULL},
      "\033[96mKernel Heap Allocator\033[0m\n"
      "  Two-tier design:\n"
      "  1. \033[93mSlab caches\033[0m for sizes 8–2048 bytes (9 caches total).\n"
      "     Each slab is one 4 KiB page carved into equal-size objects.\n"
      "  2. \033[93mLarge allocations\033[0m use PMM directly (>2048 bytes).\n"
      "  Every allocation has a 16-byte header with a magic cookie\n"
      "  (0xA110C8A7) to detect corruption on kfree.\n"
      "  All operations are protected by per-slab spinlocks.\n"
    },

    { {"virtio","virtio-blk","block","disk","storage",NULL},
      "\033[96mVirtIO Block Device Driver\033[0m\n"
      "  Implements VirtIO 1.1 specification over PCIe ECAM.\n"
      "  Uses split-ring virtqueues (desc table + avail ring + used ring).\n"
      "  QEMU exposes a virtio-blk device with:\n"
      "    -device virtio-blk-pci,drive=vblk0\n"
      "  PCIe enumeration uses ACPI MCFG table for ECAM base address.\n"
      "  Use: \033[93mdisk\033[0m command to see device capacity.\n"
    },

    { {"vfs","filesystem","file","directory","ramfs","devfs",NULL},
      "\033[96mVirtual File System (VFS)\033[0m\n"
      "  An abstraction layer allowing multiple FS implementations.\n"
      "  Current filesystems:\n"
      "  • \033[93mRamFS\033[0m — in-memory FS backed by kmalloc.\n"
      "      Supports files, directories, read/write/create/stat.\n"
      "  • \033[93mDevFS\033[0m — /dev character devices.\n"
      "      /dev/null (discard) and /dev/zero (zero bytes).\n"
      "  Use: \033[93mls /\033[0m, \033[93mcat\033[0m, \033[93mwrite\033[0m, \033[93mstat\033[0m commands.\n"
    },

    { {"gdt","segment","descriptor","ring","privilege",NULL},
      "\033[96mGlobal Descriptor Table (GDT)\033[0m\n"
      "  In 64-bit mode most segmentation is flat, but we still need\n"
      "  a valid GDT for privilege levels and the TSS.\n"
      "  Quanta's layout:\n"
      "    0x00  Null descriptor\n"
      "    0x08  Kernel Code (ring 0, 64-bit)\n"
      "    0x10  Kernel Data (ring 0)\n"
      "    0x18  User Code   (ring 3, 64-bit)\n"
      "    0x20  User Data   (ring 3)\n"
      "    0x28  TSS (128-bit system descriptor)\n"
      "  Each CPU gets its own GDT + TSS (stored in a static pool).\n"
    },

    { {"idt","interrupt","exception","fault","isr","irq",NULL},
      "\033[96mInterrupt Descriptor Table (IDT)\033[0m\n"
      "  256 entries — one for each possible interrupt vector.\n"
      "  Vectors 0-31:  CPU exceptions (some push an error code).\n"
      "  Vectors 32+:   Hardware IRQs via APIC / software interrupts.\n"
      "  All 256 stubs are auto-generated by gen_isr_stubs.py and\n"
      "  push a uniform register frame before calling isr_dispatch().\n"
      "  The 8259 PIC is remapped then fully masked; APIC delivers IRQs.\n"
    },

    { {"acpi","madt","rsdp","xsdt","hpet","mcfg",NULL},
      "\033[96mACPI (Advanced Configuration and Power Interface)\033[0m\n"
      "  Quanta uses ACPI tables found via the RSDP provided by Limine.\n"
      "  Tables parsed:\n"
      "  • XSDT / RSDT — root table (ACPI 2.0+ / 1.0)\n"
      "  • MADT ('APIC') — local APIC + I/O APIC IDs for SMP\n"
      "  • HPET — high-precision event timer (future use)\n"
      "  • MCFG — PCIe ECAM base addresses for VirtIO enumeration\n"
    },

    { {"limine","bootloader","boot","request",NULL},
      "\033[96mLibmine Bootloader\033[0m\n"
      "  Quanta is loaded by Limine v8.7.0 (stable release).\n"
      "  Limine requests used by Quanta:\n"
      "  • Framebuffer  — pixel-mode display\n"
      "  • Memory Map   — usable/reserved physical regions\n"
      "  • HHDM         — virtual offset for all physical RAM\n"
      "  • Kernel Addr  — physical and virtual kernel load address\n"
      "  • SMP          — AP descriptors + x2APIC opt-in\n"
      "  • RSDP         — ACPI root system description pointer\n"
      "  • Boot Time    — Unix timestamp at boot\n"
    },

    { {"spinlock","lock","mutex","synchronisation","atomic",NULL},
      "\033[96mSynchronisation Primitives\033[0m\n"
      "  Quanta uses \033[93mticket spinlocks\033[0m — fair FIFO ordering.\n"
      "  spinlock_acquire()        — spin until lock acquired\n"
      "  spinlock_irq_acquire()    — also disables interrupts (for ISRs)\n"
      "  spinlock_irq_release()    — re-enables interrupts if they were on\n"
      "  All PMM, heap, VFS, and scheduler operations use spinlocks.\n"
    },

    { {"serial","uart","com1","debug","output",NULL},
      "\033[96mSerial / UART Driver\033[0m\n"
      "  COM1 at I/O port 0x3F8, 38400 baud, 8N1.\n"
      "  All kprintf output goes to both serial and framebuffer.\n"
      "  In QEMU: add -serial stdio to see serial output in terminal.\n"
      "  Useful for debugging before the framebuffer is initialised.\n"
    },

    { {"keyboard","kbd","ps/2","input","key",NULL},
      "\033[96mPS/2 Keyboard Driver\033[0m\n"
      "  Handles IRQ1 from the PS/2 controller (I/O port 0x60).\n"
      "  Implements a scancode-set-1 to ASCII decoder.\n"
      "  The shell reads characters via kbd_getchar() (blocking).\n"
      "  Arrow keys produce ESC [ A/B/C/D sequences.\n"
    },

    { {"framebuffer","display","screen","font","terminal",NULL},
      "\033[96mFramebuffer Terminal\033[0m\n"
      "  Pixel-mode terminal drawn into the Limine framebuffer.\n"
      "  Uses an embedded 8×16 IBM VGA-style bitmap font.\n"
      "  Supports scrolling, 32-bit XRGB colour, and ANSI escape codes\n"
      "  (via the kprintf %s formatter — ANSI filtered in fb_putchar).\n"
    },

    { {"help","command","commands","what can","list",NULL},
      "Type \033[96mhelp\033[0m to see all built-in commands.\n"
      "Some highlights:\n"
      "  \033[93mls\033[0m, \033[93mcat\033[0m, \033[93mwrite\033[0m, \033[93mstat\033[0m  — filesystem\n"
      "  \033[93mmem\033[0m, \033[93mcpuinfo\033[0m, \033[93muptime\033[0m  — system info\n"
      "  \033[93mtasks\033[0m, \033[93msleep\033[0m          — scheduler\n"
      "  \033[93mdisk\033[0m                   — VirtIO block device\n"
      "  \033[93mai <question>\033[0m          — this assistant\n"
    },

    { {"efer","msr","nx","no-execute","sce","syscall",NULL},
      "\033[96mModel Specific Registers (MSRs)\033[0m\n"
      "  Quanta uses MSRs for:\n"
      "  • EFER (0xC0000080) — enables NX (no-execute) and SCE (syscall)\n"
      "  • APIC_BASE (0x1B) — LAPIC base address + x2APIC enable bit\n"
      "  • GS_BASE (0xC0000101) — per-CPU local data pointer\n"
      "  • x2APIC registers (0x800–0x83F) — fast LAPIC access via MSR\n"
    },

    // Catch-all
    { {NULL},
      "\033[90m[QAI] I don't have a specific answer for that.\n"
      "Try asking about: apic, smp, scheduler, paging, pmm, heap,\n"
      "virtio, vfs, gdt, idt, acpi, limine, spinlock, keyboard, or\n"
      "any other Quanta subsystem.\033[0m\n"
    },
};

#define KB_COUNT  ((int)(sizeof(kb)/sizeof(kb[0])))

// ── qai_answer ────────────────────────────────────────────────────────────
void qai_answer(const char *question) {
    // Score each entry by counting keyword hits
    int best_score = -1;
    int best_idx   = KB_COUNT - 1;   // default = catch-all

    for (int i = 0; i < KB_COUNT - 1; i++) {
        int score = 0;
        for (int k = 0; k < 8 && kb[i].keywords[k]; k++) {
            if (ci_contains(question, kb[i].keywords[k]))
                score++;
        }
        if (score > best_score) {
            best_score = score;
            best_idx   = i;
        }
    }

    shell_print("\n" ANSI_MAGENTA "[QAI] " ANSI_RESET);

    // Enrich responses that reference live system data
    if (best_idx >= 0 && best_score > 0) {
        // Prepend live context for certain topics
        const char *kw0 = kb[best_idx].keywords[0];
        if (kw0 && (ci_contains(kw0, "pmm") || ci_contains(question, "memory"))) {
            pmm_stats();
            shell_print("\n");
        }
        if (kw0 && ci_contains(question, "uptime")) {
            uint64_t ms = sched_uptime_ms();
            shell_print("Current uptime: %llu ms\n", (unsigned long long)ms);
        }
        if (ci_contains(question, "cpu") || ci_contains(question, "processor")) {
            char vendor[13]; cpu_vendor(vendor);
            struct limine_smp_response *smp = limine_smp();
            shell_print("Running on: %s  |  %u CPU(s)\n\n",
                        vendor, smp ? (uint32_t)smp->cpu_count : 1);
        }
    }

    shell_print("%s\n", kb[best_idx].response);
}
