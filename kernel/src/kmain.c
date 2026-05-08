// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.0
//
//  Boot sequence:
//    1.  Serial (pre-framebuffer debug output)
//    2.  Limine request verification
//    3.  Framebuffer terminal
//    4.  ACPI table parsing
//    5.  GDT + IDT (BSP)
//    6.  Physical memory manager
//    7.  Virtual memory manager (inherit Limine PML4, enable NX)
//    8.  Kernel heap (slab allocator)
//    9.  APIC (x2APIC or xAPIC)
//   10.  SMP — bring up all APs
//   11.  APIC timer (1 ms periodic tick, BSP)
//   12.  Scheduler
//   13.  VFS (ramfs + devfs)
//   14.  VirtIO device scan
//   15.  PS/2 Keyboard
//   16.  Launch QAI shell task
//   17.  Enable interrupts + idle loop
// ============================================================

#include "boot/limine_requests.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/isr.h"
#include "cpu/apic.h"
#include "cpu/smp.h"
#include "cpu/cpuid.h"
#include "cpu/msr.h"
#include "acpi/acpi.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "fs/vfs.h"
#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "drivers/keyboard.h"
#include "drivers/virtio/virtio.h"
#include "shell/shell.h"
#include "lib/kprintf.h"
#include "lib/string.h"

#include <stdint.h>
#include <stddef.h>

// ── Boot banner ────────────────────────────────────────────────────────────
static void print_banner(void) {
    fb_set_color(0x00D4FF, FB_COLOR_BLACK);
    kprintf(
        "\n"
        "  ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗ █████╗ \n"
        "  ██╔═══██╗██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██╔══██╗\n"
        "  ██║   ██║██║   ██║███████║██╔██╗ ██║   ██║   ███████║\n"
        "  ██║▄▄ ██║██║   ██║██╔══██║██║╚██╗██║   ██║   ██╔══██║\n"
        "  ╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║   ██║   ██║  ██║\n"
        "   ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝\n"
    );
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    kprintf(
        "  Quanta OS  v%s  (%s)\n"
        "  x2APIC  ·  SMP  ·  VirtIO  ·  VFS  ·  QAI Shell\n",
        QUANTA_VERSION, QUANTA_ARCH
    );
}

static void print_div(char c, int w) {
    for (int i=0;i<w;i++) kprintf("%c",c); kprintf("\n"); }
static void print_hdiv(void) { print_div('=', 68); }
static void print_ldiv(void) { print_div('-', 68); }

// ── Memory map printer ─────────────────────────────────────────────────────
static const char *mm_type_str(uint64_t t) {
    switch(t) {
        case LIMINE_MEMMAP_USABLE:                 return "Usable";
        case LIMINE_MEMMAP_RESERVED:               return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI Reclaim";
        case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:             return "Bad";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "BL Reclaim";
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return "Kernel+Modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:            return "Framebuffer";
        default:                                   return "Unknown";
    }
}

static void print_memmap(void) {
    struct limine_memmap_response *mm = limine_memmap();
    fb_set_color(FB_COLOR_GRAY, FB_COLOR_BLACK);
    kprintf("  %-20s  %-18s  %s\n", "Base", "Length", "Type");
    print_ldiv();
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        uint32_t color = (e->type == LIMINE_MEMMAP_USABLE) ?
                         0x00FF88 : FB_COLOR_GRAY;
        fb_set_color(color, FB_COLOR_BLACK);
        kprintf("  0x%016llx  0x%016llx  %s\n",
                (unsigned long long)e->base,
                (unsigned long long)e->length,
                mm_type_str(e->type));
    }
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
}

// ── APIC timer IRQ — feeds the scheduler ──────────────────────────────────
static void apic_timer_irq(registers_t *r) {
    (void)r;
    sched_tick();
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn))
void kmain(void) {

    // ── 1. Serial ────────────────────────────────────────────────────────
    serial_init();
    serial_write_str("\r\n[Quanta] serial up\r\n");

    // ── 2. Limine request verification ───────────────────────────────────
    if (limine_verify_requests() != 0) {
        serial_write_str("[PANIC] Limine requests not fulfilled\r\n");
        __asm__ volatile("cli"); for(;;) __asm__ volatile("hlt");
    }

    // ── 3. Framebuffer ───────────────────────────────────────────────────
    struct limine_framebuffer *fb = limine_framebuffers()->framebuffers[0];
    fb_init(fb);
    print_banner();
    print_hdiv();
    fb_set_color(FB_COLOR_GRAY, FB_COLOR_BLACK);

    // Bootloader info
    if (limine_bootloader_info()) {
        kprintf("  Bootloader : %s %s\n",
                limine_bootloader_info()->name,
                limine_bootloader_info()->version);
    }

    // Kernel addresses
    struct limine_kernel_address_response *ka = limine_kernel_addr();
    if (ka) {
        kprintf("  Kernel     : phys 0x%llx  virt 0x%llx\n",
                (unsigned long long)ka->physical_base,
                (unsigned long long)ka->virtual_base);
    }

    // Boot time
    struct limine_boot_time_response *bt = limine_boot_time();
    if (bt) kprintf("  Boot time  : %lld (Unix)\n", (long long)bt->boot_time);

    kprintf("  Framebuffer: %llux%llu  %u bpp  pitch %llu\n",
            (unsigned long long)fb->width, (unsigned long long)fb->height,
            fb->bpp, (unsigned long long)fb->pitch);
    kprintf("  HHDM offset: 0x%llx\n",
            (unsigned long long)limine_hhdm()->offset);

    // CPU info
    char vendor[13], brand[49];
    cpu_vendor(vendor); cpu_brand(brand);
    kprintf("  CPU        : %s  (%s)\n", vendor, brand);
    kprintf("  x2APIC     : %s\n", cpu_has_x2apic() ? "supported" : "no");

    print_ldiv();
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

    // ── 4. ACPI ──────────────────────────────────────────────────────────
    // Set HHDM offset early so phys_to_virt works before pmm_init
    extern uint64_t hhdm_off_early;
    hhdm_off_early = limine_hhdm()->offset;

    kprintf("[ACPI] Parsing tables...\n");
    acpi_init(limine_rsdp()->address);

    // ── 5. GDT + IDT ─────────────────────────────────────────────────────
    kprintf("[GDT] Initialising...\n");
    gdt_init();

    kprintf("[IDT] Initialising...\n");
    idt_init();

    // ── 6. PMM ───────────────────────────────────────────────────────────
    kprintf("[PMM] Initialising...\n");
    pmm_init(limine_hhdm()->offset);

    kprintf("[MEM] Physical memory map:\n");
    print_memmap();
    print_ldiv();
    pmm_stats();

    // ── 7. VMM ───────────────────────────────────────────────────────────
    kprintf("[VMM] Initialising...\n");
    vmm_init();

    // ── 8. Heap ──────────────────────────────────────────────────────────
    kprintf("[HEAP] Initialising slab allocator...\n");
    heap_init();

    // ── 9. APIC ──────────────────────────────────────────────────────────
    kprintf("[APIC] Initialising local APIC (BSP)...\n");
    apic_init();
    kprintf("[APIC] Mode: %s  LAPIC-ID: %u\n",
            apic_x2apic_mode() ? "x2APIC" : "xAPIC", apic_id());

    // ── 10. SMP ──────────────────────────────────────────────────────────
    kprintf("[SMP] Initialising...\n");
    smp_bsp_early_init();
    smp_init();

    // ── 11. APIC Timer ───────────────────────────────────────────────────
    kprintf("[APIC] Arming timer (1 ms periodic)...\n");
    irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
    apic_timer_init(1);

    // ── 12. Scheduler ────────────────────────────────────────────────────
    kprintf("[SCHED] Initialising...\n");
    sched_init();

    // ── 13. VFS ──────────────────────────────────────────────────────────
    kprintf("[VFS] Initialising...\n");
    vfs_init();

    // Create a welcome file
    {
        int fd = vfs_open("/tmp/welcome.txt", O_WRONLY | O_CREAT);
        if (fd >= 0) {
            const char *msg =
                "Welcome to Quanta OS!\n"
                "Type 'help' for available commands.\n"
                "Type 'ai <question>' to ask the QAI assistant.\n";
            vfs_write(fd, msg, strlen(msg));
            vfs_close(fd);
        }
    }

    // Write boot log to /tmp/boot.log
    {
        int fd = vfs_open("/tmp/boot.log", O_WRONLY | O_CREAT);
        if (fd >= 0) {
            char logbuf[256];
            uint32_t ncpu = limine_smp() ? (uint32_t)limine_smp()->cpu_count : 1;
            // minimal manual format
            const char *hdr = "Quanta OS boot log\n";
            vfs_write(fd, hdr, strlen(hdr));
            (void)logbuf; (void)ncpu;
            vfs_close(fd);
        }
    }

    // ── 14. VirtIO ───────────────────────────────────────────────────────
    kprintf("[VirtIO] Scanning PCIe bus...\n");
    virtio_init();

    // ── 15. Keyboard ─────────────────────────────────────────────────────
    kprintf("[KBD] Initialising PS/2 keyboard...\n");
    keyboard_init();

    // ── 16. Enable interrupts ─────────────────────────────────────────────
    __asm__ volatile("sti");
    kprintf("[CPU] Interrupts enabled\n");

    print_hdiv();
    fb_set_color(0x00FF88, FB_COLOR_BLACK);
    kprintf("  Quanta OS initialised successfully.\n");
    fb_set_color(FB_COLOR_GRAY, FB_COLOR_BLACK);
    kprintf("  %u CPU(s) online  |  %s  |  VFS ready\n",
            (uint32_t)(limine_smp() ? (uint32_t)limine_smp()->cpu_count : 1),
            apic_x2apic_mode() ? "x2APIC" : "xAPIC");
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    print_hdiv();
    kprintf("\n");

    // ── 17. Launch QAI Shell task ─────────────────────────────────────────
    task_t *shell_task = task_create("qai-shell", shell_run, NULL, 64 * 1024);
    if (!shell_task) kpanic("[INIT] Cannot create shell task\n");
    sched_add(shell_task);

    // Enable preemption on BSP and yield to scheduler
    cpu_local()->preempt_cnt = 0;
    sched_yield();

    // ── 18. Idle loop ─────────────────────────────────────────────────────
    for (;;) __asm__ volatile("hlt");
}
