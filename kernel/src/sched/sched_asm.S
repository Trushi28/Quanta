// ============================================================
//  sched/sched_asm.S — Context switch for Quanta scheduler
//
//  void sched_switch(task_ctx_t **old_ctx, task_ctx_t *new_ctx);
//
//  Saves callee-saved registers + RIP onto the current stack,
//  stores the stack pointer in *old_ctx, loads new_ctx into RSP,
//  then restores registers and returns into the new task.
// ============================================================

.intel_syntax noprefix
.code64

.global sched_switch
sched_switch:
    // Save callee-saved regs + return address (already on stack from CALL)
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    // *old_ctx (RDI) = RSP  (saves current task's context pointer)
    mov  [rdi], rsp

    // RSP = new_ctx (RDI argument)
    mov  rsp, rsi

    // Restore new task's callee-saved registers
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    // Return into the new task (RIP was pushed by its last CALL to sched_switch)
    ret

.section .note.GNU-stack,"",@progbits
