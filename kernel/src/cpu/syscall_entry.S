// ============================================================
//  cpu/syscall_entry.S — SYSCALL/SYSRET trampoline (Phase 4)
//
//  On SYSCALL from Ring 3:
//    RCX = user RIP,  R11 = user RFLAGS  (saved by CPU)
//    RAX = syscall number
//    RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
//
//  Flow:
//    swapgs → save user RSP → load kernel stack → push frame →
//    rearrange args → call syscall_dispatch → restore → swapgs → sysretq
// ============================================================

.intel_syntax noprefix
.code64

// cpu_local_t field offsets (must match smp.h)
.equ CPU_LOCAL_KERNEL_STACK, 48
.equ CPU_LOCAL_USER_RSP,     56

.section .text

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    // 1. Switch to kernel GS
    swapgs

    // 2. Save user RSP into per-CPU scratch area
    mov  gs:[CPU_LOCAL_USER_RSP], rsp

    // 3. Load kernel stack for this CPU
    mov  rsp, gs:[CPU_LOCAL_KERNEL_STACK]

    // 4. Build stack frame
    //    We save user RIP (RCX), user RFLAGS (R11), user RSP,
    //    plus callee-saved registers.
    push rcx                // user RIP
    push r11                // user RFLAGS
    push QWORD PTR gs:[CPU_LOCAL_USER_RSP]  // user RSP

    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    // 5. Rearrange arguments for C calling convention
    //    User:  RAX=num  RDI=a1  RSI=a2  RDX=a3  R10=a4  R8=a5
    //    C ABI: RDI=num  RSI=a1  RDX=a2  RCX=a3  R8=a4   R9=a5
    mov  r9,  r8            // a5
    mov  r8,  r10           // a4
    mov  rcx, rdx           // a3
    mov  rdx, rsi           // a2
    mov  rsi, rdi           // a1
    mov  rdi, rax           // syscall number

    // 6. Enable interrupts during syscall handling
    sti
    call syscall_dispatch
    cli

    // RAX = return value from syscall_dispatch (returned to user)

    // 7. Restore callee-saved registers
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    // 8. Restore user context for SYSRET
    //    Stack now has: [user RSP] [user RFLAGS] [user RIP]
    mov  rdi, [rsp]         // user RSP → RDI (temp)
    mov  r11, [rsp + 8]     // user RFLAGS → R11
    mov  rcx, [rsp + 16]    // user RIP → RCX
    add  rsp, 24            // clean up frame

    // 9. Restore user stack pointer
    mov  rsp, rdi

    // 10. Switch back to user GS
    swapgs

    // 11. Return to Ring 3
    //     SYSRET loads: RIP←RCX, RFLAGS←R11, CS←STAR[63:48]+16, SS←STAR[63:48]+8
    sysretq

.section .note.GNU-stack,"",@progbits
