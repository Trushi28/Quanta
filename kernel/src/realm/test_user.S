// ============================================================
//  realm/test_user.S — Minimal Ring 3 test binary (Phase 4)
//
//  Freestanding x86-64 program that:
//    1. Writes "Hello from Ring 3!\n" to fd=1 via SYS_WRITE
//    2. Calls SYS_EXIT(0) to terminate cleanly
//
//  Compiled by the Makefile into a static ELF64 linked at 0x400000,
//  then converted to a C byte array via xxd for embedding in the kernel.
//
//  NOTE: Everything is in .text to keep a single PT_LOAD segment.
//  Length is hardcoded to avoid GAS Intel-syntax .equ ambiguity.
// ============================================================

.intel_syntax noprefix
.code64

.section .text
.global _start

_start:
    // ── SYS_WRITE(1, msg, 19) ──────────────────────────────
    mov  rax, 1             // SYS_WRITE
    mov  rdi, 1             // fd = stdout
    lea  rsi, [rip + msg]   // buf = message (RIP-relative)
    mov  rdx, 19            // len = 19 bytes (hardcoded)
    syscall

    // ── SYS_EXIT(0) ────────────────────────────────────────
    mov  rax, 60            // SYS_EXIT
    xor  rdi, rdi           // exit code 0
    syscall

    // Should never reach here
    ud2

msg:
    .ascii "Hello from Ring 3!\n"
    // len = 19 bytes: "Hello from Ring 3!\n"

.section .note.GNU-stack,"",@progbits
