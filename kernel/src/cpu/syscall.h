#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  cpu/syscall.h — Quanta OS Syscall Interface (Phase 4)
//
//  13 syscalls total.  Everything else is handled in Ring 3 by the LibOS.
//  Argument convention follows Linux x86-64 ABI:
//    RAX=number, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
// ---------------------------------------------------------------------------

// ── Syscall numbers ───────────────────────────────────────────────────────
// Core I/O
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_YIELD          24
#define SYS_SLEEP          35
#define SYS_GETPID         39
#define SYS_EXIT           60

// Memory management
#define SYS_PAGE_REQUEST  200
#define SYS_PAGE_RELEASE  201

// Inter-Realm Communication
#define SYS_IPC_SEND      202
#define SYS_IPC_RECV      203

// Realm operations
#define SYS_REALM_ID      204
#define SYS_REALM_EXIT    205
#define SYS_LIBOS_FETCH   206

// ── Error codes (negative return values) ──────────────────────────────────
#define EQUANTA_PERM    4    // permission denied
#define EQUANTA_NOENT   2    // no such entry
#define EQUANTA_INVAL  22    // invalid argument
#define EQUANTA_NOSYS  38    // syscall not implemented
#define EQUANTA_FAULT  14    // bad address
#define EQUANTA_NOMEM  12    // out of memory

// ── Initialisation ────────────────────────────────────────────────────────
// Program STAR, LSTAR, FMASK MSRs.  Must be called on every CPU.
void syscall_init(void);

// C dispatch function called from the assembly trampoline.
int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);
