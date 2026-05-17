// ============================================================
//  cpu/syscall.c — SYSCALL MSR setup + syscall_dispatch (Phase 4)
//
//  Programs STAR, LSTAR, FMASK MSRs for fast Ring 3 ↔ Ring 0
//  transitions.  Implements the Quanta Ring 3 syscall surface.
// ============================================================
#include "syscall.h"
#include "gdt.h"
#include "msr.h"
#include "power.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../fs/vfs.h"
#include "../realm/realm.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../drivers/framebuffer.h"
#include <stdint.h>
#include <stddef.h>

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

static int valid_user_pages(uint64_t addr, uint64_t n_pages) {
    if (n_pages == 0) return 0;
    if (addr & (PAGE_SIZE - 1)) return 0;
    if (n_pages > ((USER_ADDR_MAX + 1) / PAGE_SIZE)) return 0;
    return valid_user_ptr(addr, n_pages * PAGE_SIZE);
}

static int current_has_cap(uint32_t cap) {
    realm_t *r = realm_current();
    return r && ((r->caps & cap) == cap);
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

static int copy_to_user(uint64_t user_addr, const void *src, size_t len) {
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;

    const uint8_t *s = (const uint8_t *)src;
    size_t copied = 0;

    while (copied < len) {
        uint64_t page_off = (user_addr + copied) & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - copied) chunk = len - copied;

        uint64_t phys = vmm_virt_to_phys(cur->page_table, user_addr + copied);
        if (!phys) return -EQUANTA_FAULT;

        void *kptr = phys_to_virt(phys);
        memcpy(kptr, s + copied, chunk);
        copied += chunk;
    }
    return 0;
}

static int copy_user_string(char *dst, uint64_t user_addr, size_t max) {
    task_t *cur = sched_current();
    if (!cur || !cur->page_table || !dst || max == 0)
        return -EQUANTA_FAULT;

    for (size_t i = 0; i < max; i++) {
        if (!valid_user_ptr(user_addr + i, 1))
            return -EQUANTA_FAULT;
        uint64_t phys = vmm_virt_to_phys(cur->page_table, user_addr + i);
        if (!phys)
            return -EQUANTA_FAULT;
        char c = *(char *)phys_to_virt(phys);
        dst[i] = c;
        if (c == '\0')
            return 0;
    }

    dst[max - 1] = '\0';
    return -EQUANTA_INVAL;
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
    if (!current_has_cap(CAP_VFS)) return -EQUANTA_PERM;
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
    if (!current_has_cap(CAP_VFS)) return -EQUANTA_PERM;
    if (!valid_user_ptr(user_buf, len)) return -EQUANTA_FAULT;
    if (len == 0) return 0;
    if (len > 4096) len = 4096;

    if (fd == 0) {
        char c = kbd_getchar();
        int rc = copy_to_user(user_buf, &c, 1);
        return rc < 0 ? rc : 1;
    }

    char kbuf[4096];
    ssize_t n = vfs_read((int)fd, kbuf, len);
    if (n <= 0) return (int64_t)n;

    int rc = copy_to_user(user_buf, kbuf, (size_t)n);
    return rc < 0 ? rc : (int64_t)n;
}

static int64_t sys_open(uint64_t user_path, uint64_t flags) {
    if (!current_has_cap(CAP_VFS)) return -EQUANTA_PERM;
    char path[VFS_PATH_MAX];
    int rc = copy_user_string(path, user_path, sizeof(path));
    if (rc < 0) return rc;
    int fd = vfs_open(path, (int)flags);
    return fd < 0 ? -EQUANTA_NOENT : (int64_t)fd;
}

static int64_t sys_close(uint64_t fd) {
    if (!current_has_cap(CAP_VFS)) return -EQUANTA_PERM;
    int rc = vfs_close((int)fd);
    return rc < 0 ? -EQUANTA_INVAL : 0;
}

static int64_t sys_stat(uint64_t user_path, uint64_t user_stat) {
    if (!current_has_cap(CAP_VFS)) return -EQUANTA_PERM;
    if (!valid_user_ptr(user_stat, sizeof(vfs_stat_t))) return -EQUANTA_FAULT;

    char path[VFS_PATH_MAX];
    int rc = copy_user_string(path, user_path, sizeof(path));
    if (rc < 0) return rc;

    vfs_stat_t st;
    rc = vfs_stat2(path, &st);
    if (rc < 0) return -EQUANTA_NOENT;

    rc = copy_to_user(user_stat, &st, sizeof(st));
    return rc < 0 ? rc : 0;
}

static int64_t sys_readdir(uint64_t fd, uint64_t idx, uint64_t user_name) {
    if (!current_has_cap(CAP_VFS)) return -EQUANTA_PERM;
    if (!valid_user_ptr(user_name, VFS_NAME_MAX)) return -EQUANTA_FAULT;

    char name[VFS_NAME_MAX];
    int rc = vfs_readdir((int)fd, (uint32_t)idx, name);
    if (rc < 0) return -EQUANTA_NOENT;

    rc = copy_to_user(user_name, name, sizeof(name));
    return rc < 0 ? rc : 0;
}

static int64_t sys_page_release(uint64_t vaddr, uint64_t n_pages);

static int64_t sys_page_request(uint64_t vaddr, uint64_t n_pages, uint64_t flags) {
    (void)flags;
    if (!current_has_cap(CAP_PAGES)) return -EQUANTA_PERM;
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;
    if (!valid_user_pages(vaddr, n_pages)) return -EQUANTA_INVAL;

    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t va = vaddr + i * PAGE_SIZE;
        if (vmm_virt_to_phys(cur->page_table, va)) {
            sys_page_release(vaddr, i);
            return -EQUANTA_INVAL;
        }

        uint64_t phys = pmm_alloc();
        if (!phys) {
            sys_page_release(vaddr, i);
            return -EQUANTA_NOMEM;
        }
        // Zero the page
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        int rc = vmm_map_page(cur->page_table, va, phys, VMM_USER_RW);
        if (rc != 0) {
            pmm_free(phys);
            sys_page_release(vaddr, i);
            return -EQUANTA_NOMEM;
        }
    }
    return 0;
}

static int64_t sys_page_release(uint64_t vaddr, uint64_t n_pages) {
    if (!current_has_cap(CAP_PAGES)) return -EQUANTA_PERM;
    task_t *cur = sched_current();
    if (!cur || !cur->page_table) return -EQUANTA_FAULT;
    if (!valid_user_pages(vaddr, n_pages)) return -EQUANTA_INVAL;

    for (uint64_t i = 0; i < n_pages; i++) {
        uint64_t va = vaddr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(cur->page_table, va);
        vmm_unmap_page(cur->page_table, va);
        if (phys) pmm_free(phys & ~(PAGE_SIZE - 1));
    }
    return 0;
}

static int64_t sys_libos_fetch(uint64_t type, uint64_t user_name,
                               uint64_t len) {
    if (type > REALM_WIN32)
        return -EQUANTA_INVAL;
    if (len == 0 || len >= LIBOS_MODULE_NAME_MAX)
        return -EQUANTA_INVAL;
    if (!valid_user_ptr(user_name, len))
        return -EQUANTA_FAULT;
    if (!realm_current())
        return -EQUANTA_FAULT;

    char name[LIBOS_MODULE_NAME_MAX];
    memset(name, 0, sizeof(name));
    int rc = copy_from_user(name, user_name, (size_t)len);
    if (rc < 0)
        return rc;
    name[LIBOS_MODULE_NAME_MAX - 1] = '\0';

    const libos_module_t *m = libos_fetch_module((realm_type_t)type, name);
    if (!m)
        return -EQUANTA_NOENT;

    return (int64_t)m->id;
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

    case SYS_OPEN:
        return sys_open(a1, a2);

    case SYS_CLOSE:
        return sys_close(a1);

    case SYS_STAT:
        return sys_stat(a1, a2);

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
        realm_t *r = realm_current();
        if (!r) return -EQUANTA_FAULT;
        return (int64_t)r->id;
    }

    case SYS_REALM_EXIT:
        return (int64_t)realm_destroy_current();

    case SYS_LIBOS_FETCH:
        return sys_libos_fetch(a1, a2, a3);

    case SYS_REBOOT:
        if (!current_has_cap(CAP_POWER)) return -EQUANTA_PERM;
        power_reboot();
        __builtin_unreachable();

    case SYS_SHUTDOWN:
        if (!current_has_cap(CAP_POWER)) return -EQUANTA_PERM;
        power_shutdown();
        __builtin_unreachable();

    case SYS_READDIR:
        return sys_readdir(a1, a2, a3);

    default:
        return -EQUANTA_NOSYS;
    }
}
