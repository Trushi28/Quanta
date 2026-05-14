// ============================================================
//  cpu/syscall.c — SYSCALL MSR setup + syscall_dispatch (Phase 4)
//
//  Programs STAR, LSTAR, FMASK MSRs for fast Ring 3 ↔ Ring 0
//  transitions.  Implements all 13 Quanta syscalls.
// ============================================================
#include "syscall.h"
#include "gdt.h"
#include "msr.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../fs/vfs.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include <stdint.h>
#include <stddef.h>

// Forward declarations — resolved after realm.h exists
struct realm;
extern struct realm *realm_current(void);
extern int realm_destroy_current(void);

// Assembly trampoline
extern void syscall_entry(void);

// ── User address validation ──────────────────────────────────────────────
#define USER_ADDR_MAX  0x00007FFFFFFFFFFFULL

static int valid_user_ptr(uint64_t addr, uint64_t len) {
    if (addr > USER_ADDR_MAX) return 0;
    if (addr + len < addr) return 0;   // overflow
    if (addr + len > USER_ADDR_MAX + 1) return 0;
    return 1;
}

// ── Copy from user space to kernel buffer ─────────────────────────────────
// Uses vmm_virt_to_phys() + phys_to_virt() (HHDM) to access user memory.
static int copy_from_user(void *dst, uint64_t user_addr, size_t len) {
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;

    uint8_t *d = (uint8_t *)dst;
    size_t copied = 0;

    while (copied < len) {
        uint64_t page_off = (user_addr + copied) & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - copied) chunk = len - copied;

        uint64_t phys = vmm_virt_to_phys(cur->page_table, user_addr + copied);
        if (!phys) return -EQUANTA_FAULT;

        void *kptr = phys_to_virt(phys);
        memcpy(d + copied, kptr, chunk);
        copied += chunk;
    }
    return 0;
}

// ── syscall_init ──────────────────────────────────────────────────────────
void syscall_init(void) {
    // STAR MSR:
    //   bits [47:32] = SYSCALL CS/SS base  → 0x08 (kernel code at 0x08, kernel data at 0x10)
    //   bits [63:48] = SYSRET  CS/SS base  → 0x10 (SS=0x10+8=0x18 udata, CS=0x10+16=0x20 ucode)
    uint64_t star = ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(MSR_STAR, star);

    // LSTAR = kernel entry point for SYSCALL
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    // CSTAR not used in 64-bit mode but zero it for safety
    wrmsr(MSR_CSTAR, 0);

    // FMASK = bits to CLEAR in RFLAGS on SYSCALL entry
    //   Clear IF (bit 9) to disable interrupts on entry
    wrmsr(MSR_FMASK, 0x200);

    kprintf("[SYSCALL] MSRs programmed  STAR=0x%llx  LSTAR=0x%llx\n",
            (unsigned long long)star,
            (unsigned long long)(uintptr_t)syscall_entry);
}

// ── Syscall handlers ──────────────────────────────────────────────────────

static int64_t sys_write(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (!valid_user_ptr(user_buf, len)) return -EQUANTA_FAULT;
    if (len == 0) return 0;
    if (len > 4096) len = 4096;  // cap per-call length

    char kbuf[4096];
    int rc = copy_from_user(kbuf, user_buf, len);
    if (rc < 0) return rc;

    if (fd == 1 || fd == 2) {
        // stdout/stderr → serial + framebuffer
        for (size_t i = 0; i < len; i++) {
            serial_write_char(kbuf[i]);
            fb_putchar(kbuf[i]);
        }
        return (int64_t)len;
    }

    // Real fd — delegate to VFS
    return (int64_t)vfs_write((int)fd, kbuf, len);
}

static int64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (!valid_user_ptr(user_buf, len)) return -EQUANTA_FAULT;
    if (len == 0) return 0;
    if (len > 4096) len = 4096;

    char kbuf[4096];
    ssize_t n = vfs_read((int)fd, kbuf, len);
    if (n <= 0) return (int64_t)n;

    // Copy back to user — need to write to user pages via HHDM
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;

    size_t written = 0;
    while (written < (size_t)n) {
        uint64_t page_off = (user_buf + written) & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > (size_t)n - written) chunk = (size_t)n - written;

        uint64_t phys = vmm_virt_to_phys(cur->page_table, user_buf + written);
        if (!phys) return -EQUANTA_FAULT;

        void *kptr = phys_to_virt(phys);
        memcpy(kptr, kbuf + written, chunk);
        written += chunk;
    }

    return (int64_t)n;
}

static int64_t sys_page_request(uint64_t vaddr, uint64_t n_pages, uint64_t flags) {
    (void)flags;
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;
    if (!valid_user_ptr(vaddr, n_pages * PAGE_SIZE)) return -EQUANTA_INVAL;
    if (vaddr & (PAGE_SIZE - 1)) return -EQUANTA_INVAL;  // must be page-aligned

    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) return -EQUANTA_NOMEM;
        // Zero the page
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        int rc = vmm_map_page(cur->page_table, vaddr + i * PAGE_SIZE, phys, VMM_USER_RW);
        if (rc != 0) {
            pmm_free(phys);
            return -EQUANTA_NOMEM;
        }
    }
    return 0;
}

static int64_t sys_page_release(uint64_t vaddr, uint64_t n_pages) {
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;
    if (!valid_user_ptr(vaddr, n_pages * PAGE_SIZE)) return -EQUANTA_INVAL;

    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t va = vaddr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(cur->page_table, va);
        vmm_unmap_page(cur->page_table, va);
        if (phys) pmm_free(phys & ~(PAGE_SIZE - 1));
    }
    return 0;
}

// ── syscall_dispatch ──────────────────────────────────────────────────────
int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    switch (num) {
    case SYS_READ:
        return sys_read(a1, a2, a3);

    case SYS_WRITE:
        return sys_write(a1, a2, a3);

    case SYS_YIELD:
        sched_yield();
        return 0;

    case SYS_SLEEP:
        sched_sleep_ms(a1);
        return 0;

    case SYS_GETPID:
        return (int64_t)sched_current()->pid;

    case SYS_EXIT:
        sched_exit((int)a1);
        // noreturn — but compiler needs this
        __builtin_unreachable();

    case SYS_PAGE_REQUEST:
        return sys_page_request(a1, a2, a3);

    case SYS_PAGE_RELEASE:
        return sys_page_release(a1, a2);

    case SYS_IPC_SEND:
    case SYS_IPC_RECV:
        return -EQUANTA_NOSYS;  // Phase 7+

    case SYS_REALM_ID: {
        struct realm *r = realm_current();
        if (!r) return -EQUANTA_FAULT;
        // realm_id is the first field — we'll cast once realm.h is included
        return (int64_t)(*(uint32_t *)r);
    }

    case SYS_REALM_EXIT:
        return (int64_t)realm_destroy_current();

    case SYS_LIBOS_FETCH:
        return -EQUANTA_NOSYS;  // Phase 5+

    default:
        return -EQUANTA_NOSYS;
    }
}
