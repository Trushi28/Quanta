// ============================================================
//  realm/test_user.S — Ring 3 Native Init Console (Phase 4)
//
//  Freestanding x86-64 userspace program.  It proves Quanta can
//  run a persistent user-facing task in Ring 3 using only syscalls:
//    - SYS_WRITE: console output
//    - SYS_READ: keyboard input from fd 0
//    - SYS_GETPID / SYS_REALM_ID
//    - SYS_PAGE_REQUEST / SYS_PAGE_RELEASE
//    - SYS_LIBOS_FETCH
// ============================================================

.intel_syntax noprefix
.code64

.equ SYS_READ,         0
.equ SYS_WRITE,        1
.equ SYS_OPEN,         2
.equ SYS_CLOSE,        3
.equ SYS_STAT,         4
.equ SYS_GETPID,       39
.equ SYS_EXIT,         60
.equ SYS_PAGE_REQUEST, 200
.equ SYS_PAGE_RELEASE, 201
.equ SYS_REALM_ID,     204
.equ SYS_LIBOS_FETCH,  206
.equ SYS_REBOOT,       207
.equ SYS_SHUTDOWN,     208
.equ SYS_READDIR,      209

.equ REALM_WASM,       1
.equ SCRATCH_PAGE,     0x500000

.section .text
.global _start

_start:
    lea  rdi, [rip + banner]
    call puts

main_loop:
    lea  rdi, [rip + prompt]
    call puts
    call read_line
    lea  rdi, [rip + cmd_buf]
    cmp  BYTE PTR [rdi], 0
    je   main_loop

    lea  rdi, [rip + cmd_help]
    call cmd_eq
    test eax, eax
    jnz  do_help

    lea  rdi, [rip + cmd_pid]
    call cmd_eq
    test eax, eax
    jnz  do_pid

    lea  rdi, [rip + cmd_realm]
    call cmd_eq
    test eax, eax
    jnz  do_realm

    lea  rdi, [rip + cmd_libos]
    call cmd_eq
    test eax, eax
    jnz  do_libos

    lea  rdi, [rip + cmd_ls]
    call cmd_eq
    test eax, eax
    jnz  do_ls

    lea  rdi, [rip + cmd_cat]
    call cmd_eq
    test eax, eax
    jnz  do_cat

    lea  rdi, [rip + cmd_wasm]
    call cmd_eq
    test eax, eax
    jnz  do_wasm

    lea  rdi, [rip + cmd_page]
    call cmd_eq
    test eax, eax
    jnz  do_page

    lea  rdi, [rip + cmd_game]
    call cmd_eq
    test eax, eax
    jnz  do_game

    lea  rdi, [rip + cmd_reboot]
    call cmd_eq
    test eax, eax
    jnz  do_reboot

    lea  rdi, [rip + cmd_shutdown]
    call cmd_eq
    test eax, eax
    jnz  do_shutdown

    lea  rdi, [rip + cmd_exit]
    call cmd_eq
    test eax, eax
    jnz  do_exit

    lea  rdi, [rip + unknown]
    call puts
    jmp  main_loop

do_help:
    lea  rdi, [rip + help_text]
    call puts
    jmp  main_loop

do_pid:
    lea  rdi, [rip + pid_prefix]
    call puts
    mov  rax, SYS_GETPID
    syscall
    mov  rdi, rax
    call put_u64
    call newline
    jmp  main_loop

do_realm:
    lea  rdi, [rip + realm_prefix]
    call puts
    mov  rax, SYS_REALM_ID
    syscall
    mov  rdi, rax
    call put_u64
    call newline
    jmp  main_loop

do_libos:
    lea  rdi, [rip + libos_prefix]
    call puts
    mov  rax, SYS_LIBOS_FETCH
    mov  rdi, REALM_WASM
    lea  rsi, [rip + wasm_module]
    mov  rdx, 15
    syscall
    test rax, rax
    js   libos_fail
    mov  rdi, rax
    call put_u64
    call newline
    jmp  main_loop

libos_fail:
    lea  rdi, [rip + fail_text]
    call puts
    jmp  main_loop

do_ls:
    lea  rdi, [rip + root_path]
    call list_dir
    lea  rdi, [rip + apps_path]
    call list_dir
    jmp  main_loop

do_cat:
    lea  rdi, [rip + welcome_path]
    call cat_file
    jmp  main_loop

do_wasm:
    lea  rdi, [rip + hello_wasm_path]
    call wasm_probe
    jmp  main_loop

do_page:
    mov  rax, SYS_PAGE_REQUEST
    mov  rdi, SCRATCH_PAGE
    mov  rsi, 1
    xor  rdx, rdx
    syscall
    test rax, rax
    js   page_fail

    mov  BYTE PTR [SCRATCH_PAGE], 'Q'

    mov  rax, SYS_PAGE_RELEASE
    mov  rdi, SCRATCH_PAGE
    mov  rsi, 1
    syscall
    test rax, rax
    js   page_fail

    lea  rdi, [rip + page_ok]
    call puts
    jmp  main_loop

page_fail:
    lea  rdi, [rip + fail_text]
    call puts
    jmp  main_loop

do_game:
    lea  rdi, [rip + game_text]
    call puts
.game_read:
    mov  rax, SYS_READ
    xor  rdi, rdi
    lea  rsi, [rip + input_ch]
    mov  rdx, 1
    syscall
    test rax, rax
    jle  .game_read
    mov  al, BYTE PTR [rip + input_ch]
    mov  BYTE PTR [rip + echo_ch], al
    lea  rdi, [rip + echo_ch]
    mov  rsi, 1
    call write_len
    call newline
    mov  al, BYTE PTR [rip + input_ch]
    cmp  al, '4'
    je   .win
    lea  rdi, [rip + game_miss]
    call puts
    jmp  main_loop
.win:
    lea  rdi, [rip + game_win]
    call puts
    jmp  main_loop

do_reboot:
    lea  rdi, [rip + reboot_text]
    call puts
    mov  rax, SYS_REBOOT
    syscall
    lea  rdi, [rip + fail_text]
    call puts
    jmp  main_loop

do_shutdown:
    lea  rdi, [rip + shutdown_text]
    call puts
    mov  rax, SYS_SHUTDOWN
    syscall
    lea  rdi, [rip + fail_text]
    call puts
    jmp  main_loop

do_exit:
    lea  rdi, [rip + exit_text]
    call puts
    mov  rax, SYS_EXIT
    xor  rdi, rdi
    syscall
    ud2

// ------------------------------------------------------------
// list_dir: rdi = absolute path. Uses SYS_OPEN + SYS_READDIR.
// ------------------------------------------------------------
list_dir:
    push r12
    push r13
    push r14
    mov  r12, rdi

    lea  rdi, [rip + list_prefix]
    call puts
    mov  rdi, r12
    call puts
    call newline

    mov  rax, SYS_OPEN
    mov  rdi, r12
    xor  rsi, rsi
    syscall
    test rax, rax
    js   list_fail

    mov  r14, rax
    xor  r13d, r13d
list_next:
    mov  rax, SYS_READDIR
    mov  rdi, r14
    mov  rsi, r13
    lea  rdx, [rip + dir_name]
    syscall
    test rax, rax
    js   list_done

    lea  rdi, [rip + list_item]
    call puts
    lea  rdi, [rip + dir_name]
    call puts
    call newline
    inc  r13
    jmp  list_next

list_done:
    mov  rax, SYS_CLOSE
    mov  rdi, r14
    syscall
    pop  r14
    pop  r13
    pop  r12
    ret
list_fail:
    lea  rdi, [rip + fail_text]
    call puts
    pop  r14
    pop  r13
    pop  r12
    ret

// ------------------------------------------------------------
// cat_file: rdi = absolute path. Streams file to stdout.
// ------------------------------------------------------------
cat_file:
    push r12
    mov  r12, rdi
    mov  rax, SYS_OPEN
    mov  rdi, r12
    xor  rsi, rsi
    syscall
    test rax, rax
    js   cat_fail
    mov  r12, rax

cat_read:
    mov  rax, SYS_READ
    mov  rdi, r12
    lea  rsi, [rip + file_buf]
    mov  rdx, 192
    syscall
    test rax, rax
    js   cat_close_fail
    jz   cat_close_ok
    lea  rdi, [rip + file_buf]
    mov  rsi, rax
    call write_len
    jmp  cat_read

cat_close_ok:
    call newline
    mov  rax, SYS_CLOSE
    mov  rdi, r12
    syscall
    pop  r12
    ret
cat_close_fail:
    mov  rax, SYS_CLOSE
    mov  rdi, r12
    syscall
cat_fail:
    lea  rdi, [rip + fail_text]
    call puts
    pop  r12
    ret

// ------------------------------------------------------------
// wasm_probe: rdi = absolute path. Validates magic and routes to LibOS.
// ------------------------------------------------------------
wasm_probe:
    push r12
    push r13
    mov  r12, rdi

    mov  rax, SYS_OPEN
    mov  rdi, r12
    xor  rsi, rsi
    syscall
    test rax, rax
    js   wasm_fail
    mov  r12, rax

    mov  rax, SYS_READ
    mov  rdi, r12
    lea  rsi, [rip + file_buf]
    mov  rdx, 64
    syscall
    mov  r13, rax

    mov  rax, SYS_CLOSE
    mov  rdi, r12
    syscall

    cmp  r13, 8
    jl   wasm_fail
    cmp  BYTE PTR [rip + file_buf + 0], 0
    jne  wasm_not_wasm
    cmp  BYTE PTR [rip + file_buf + 1], 'a'
    jne  wasm_not_wasm
    cmp  BYTE PTR [rip + file_buf + 2], 's'
    jne  wasm_not_wasm
    cmp  BYTE PTR [rip + file_buf + 3], 'm'
    jne  wasm_not_wasm
    cmp  BYTE PTR [rip + file_buf + 4], 1
    jne  wasm_not_wasm

    lea  rdi, [rip + wasm_ok]
    call puts
    mov  rax, SYS_LIBOS_FETCH
    mov  rdi, REALM_WASM
    lea  rsi, [rip + wasm_module]
    mov  rdx, 15
    syscall
    test rax, rax
    js   wasm_fail
    lea  rdi, [rip + libos_prefix]
    call puts
    mov  rdi, rax
    call put_u64
    call newline

    cmp  r13, 33
    jl   wasm_done
    lea  rdi, [rip + wasm_sample]
    call puts
    lea  rdi, [rip + file_buf + 17]
    mov  rsi, 16
    call write_len
wasm_done:
    pop  r13
    pop  r12
    ret
wasm_not_wasm:
    lea  rdi, [rip + wasm_bad]
    call puts
    pop  r13
    pop  r12
    ret
wasm_fail:
    lea  rdi, [rip + fail_text]
    call puts
    pop  r13
    pop  r12
    ret

// ------------------------------------------------------------
// read_line: fills cmd_buf, echoes input, handles backspace.
// ------------------------------------------------------------
read_line:
    xor  r12d, r12d
.read_next:
    mov  rax, SYS_READ
    xor  rdi, rdi
    lea  rsi, [rip + input_ch]
    mov  rdx, 1
    syscall
    test rax, rax
    jle  .read_next

    mov  al, BYTE PTR [rip + input_ch]
    cmp  al, 13
    je   .done
    cmp  al, 10
    je   .done
    cmp  al, 8
    je   .backspace
    cmp  al, 127
    je   .backspace

    cmp  r12, 63
    jae  .read_next
    lea  rbx, [rip + cmd_buf]
    mov  BYTE PTR [rbx + r12], al
    inc  r12
    mov  BYTE PTR [rip + echo_ch], al
    lea  rdi, [rip + echo_ch]
    mov  rsi, 1
    call write_len
    jmp  .read_next

.backspace:
    test r12, r12
    jz   .read_next
    dec  r12
    lea  rdi, [rip + bs_seq]
    call puts
    jmp  .read_next

.done:
    lea  rbx, [rip + cmd_buf]
    mov  BYTE PTR [rbx + r12], 0
    call newline
    ret

// ------------------------------------------------------------
// cmd_eq: rdi = null-terminated literal, compares to cmd_buf.
// returns eax=1 on match.
// ------------------------------------------------------------
cmd_eq:
    lea  rsi, [rip + cmd_buf]
.cmp:
    mov  al, BYTE PTR [rdi]
    mov  bl, BYTE PTR [rsi]
    cmp  al, bl
    jne  .no
    test al, al
    jz   .yes
    inc  rdi
    inc  rsi
    jmp  .cmp
.yes:
    mov  eax, 1
    ret
.no:
    xor  eax, eax
    ret

puts:
    push rdi
    xor  rsi, rsi
.len:
    cmp  BYTE PTR [rdi + rsi], 0
    je   .write
    inc  rsi
    jmp  .len
.write:
    pop  rdi
    jmp  write_len

write_len:
    mov  rdx, rsi
    mov  rsi, rdi
    mov  rax, SYS_WRITE
    mov  rdi, 1
    syscall
    ret

newline:
    lea  rdi, [rip + nl]
    mov  rsi, 1
    jmp  write_len

put_u64:
    lea  rsi, [rip + num_buf + 31]
    mov  BYTE PTR [rsi], 0
    mov  rax, rdi
    mov  rbx, 10
    test rax, rax
    jnz  .digits
    dec  rsi
    mov  BYTE PTR [rsi], '0'
    jmp  .out
.digits:
    xor  rdx, rdx
    div  rbx
    add  dl, '0'
    dec  rsi
    mov  BYTE PTR [rsi], dl
    test rax, rax
    jnz  .digits
.out:
    mov  rdi, rsi
    call puts
    ret

.section .rodata
banner:
    .asciz "\n[Ring3 Init] Quanta Native Realm console online.\nType 'help' for user-mode commands.\n"
prompt:
    .asciz "r3:/ $ "
help_text:
    .asciz "commands: help, ls, cat, wasm, pid, realm, libos, page, game, reboot, shutdown, exit\n  ls       -> SYS_OPEN + SYS_READDIR on / and /apps\n  cat      -> read /welcome.txt through Ring 3 VFS syscalls\n  wasm     -> validate /apps/hello.wasm and route to WASI LibOS\n  pid      -> SYS_GETPID\n  realm    -> SYS_REALM_ID\n  libos    -> SYS_LIBOS_FETCH(wasm,wasi_runtime.so)\n  page     -> map/write/unmap one user page\n  game     -> tiny Ring 3 number game\n  reboot   -> SYS_REBOOT (init capability)\n  shutdown -> SYS_SHUTDOWN (init capability)\n"
unknown:
    .asciz "unknown command; try 'help'\n"
pid_prefix:
    .asciz "pid="
realm_prefix:
    .asciz "realm="
libos_prefix:
    .asciz "wasi_runtime.so module id="
list_prefix:
    .asciz "directory "
list_item:
    .asciz "  - "
wasm_ok:
    .asciz "WASM magic/version OK: /apps/hello.wasm -> WASM Realm route\n"
wasm_bad:
    .asciz "not a supported WASM v1 module\n"
wasm_sample:
    .asciz "sample payload: "
page_ok:
    .asciz "page syscall OK: mapped, wrote, released\n"
game_text:
    .asciz "Ring 3 game: guess the boot digit [0-9]: "
game_win:
    .asciz "correct. tiny game completed from Ring 3.\n"
game_miss:
    .asciz "not this time. hint: Foundation phase number.\n"
fail_text:
    .asciz "syscall failed\n"
reboot_text:
    .asciz "requesting reboot from Ring 3...\n"
shutdown_text:
    .asciz "requesting shutdown from Ring 3...\n"
exit_text:
    .asciz "leaving Ring 3 init console\n"
cmd_help:
    .asciz "help"
cmd_pid:
    .asciz "pid"
cmd_realm:
    .asciz "realm"
cmd_libos:
    .asciz "libos"
cmd_ls:
    .asciz "ls"
cmd_cat:
    .asciz "cat"
cmd_wasm:
    .asciz "wasm"
cmd_page:
    .asciz "page"
cmd_game:
    .asciz "game"
cmd_reboot:
    .asciz "reboot"
cmd_shutdown:
    .asciz "shutdown"
cmd_exit:
    .asciz "exit"
wasm_module:
    .asciz "wasi_runtime.so"
root_path:
    .asciz "/"
apps_path:
    .asciz "/apps"
welcome_path:
    .asciz "/welcome.txt"
hello_wasm_path:
    .asciz "/apps/hello.wasm"
nl:
    .ascii "\n"
bs_seq:
    .asciz "\b \b"

.section .bss
.align 16
cmd_buf:
    .skip 64
num_buf:
    .skip 32
dir_name:
    .skip 64
file_buf:
    .skip 256
input_ch:
    .skip 1
echo_ch:
    .skip 1

.section .note.GNU-stack,"",@progbits
