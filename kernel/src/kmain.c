// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.3  (Phase 3)
//
//  Phase 3 additions:
//    • kv_init() called after virtio_init() — enables the
//      persistent key-value store in the `kv` shell command
//    • Status bar updated to mention kv and new commands
// ============================================================

#include "acpi/acpi.h"
#include "boot/limine_requests.h"
#include "cpu/apic.h"
#include "cpu/cpuid.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/ioapic.h"
#include "cpu/isr.h"
#include "cpu/msr.h"
#include "cpu/smp.h"
#include "drivers/framebuffer.h"
#include "drivers/keyboard.h"
#include "drivers/kvstore.h"
#include "drivers/serial.h"
#include "drivers/virtio/virtio.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "shell/shell.h"

#include <stddef.h>
#include <stdint.h>

// ── APIC timer IRQ → scheduler tick ──────────────────────────────────────
static void apic_timer_irq(registers_t *r) {
    (void)r;
    sched_tick();
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void kmain(void) {

    // 1. Serial
    serial_init();
    serial_write_str("\r\n[Quanta] serial up\r\n");

    // 2. Limine verification
    if (limine_verify_requests() != 0) {
        serial_write_str("[PANIC] Limine requests not fulfilled\r\n");
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    // 3. Framebuffer + header panel
    struct limine_framebuffer *fb = limine_framebuffers()->framebuffers[0];
    fb_init(fb);

    fb_set_color(FB_COLOR_BLACK, 0x0D1F35);
    for (uint32_t c = 0; c < 80; c++) fb_putchar(' ');

    fb_set_color(0x00D4FF, 0x0D1F35);
    kprintf("   ____                    _       __  ____   _____ \n"
            "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
            " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
            "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
            "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n");

    fb_set_color(0xAADDFF, 0x0D1F35);
    kprintf("  Quanta OS  v%s  (%s)   "
            "x2APIC | SMP | VirtIO | QuantaFS | KV | QAI\n",
            QUANTA_VERSION, QUANTA_ARCH);

    fb_draw_hline((char)0xCD, 0x335577, 0x000000);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

    fb_set_color(0x778899, FB_COLOR_BLACK);
    if (limine_bootloader_info())
        kprintf("  Bootloader : %s %s\n",
                limine_bootloader_info()->name,
                limine_bootloader_info()->version);

    struct limine_kernel_address_response *ka = limine_kernel_addr();
    if (ka)
        kprintf("  Kernel     : phys 0x%llx  virt 0x%llx\n",
                (unsigned long long)ka->physical_base,
                (unsigned long long)ka->virtual_base);

    struct limine_boot_time_response *bt = limine_boot_time();
    if (bt)
        kprintf("  Boot time  : %lld (Unix)\n", (long long)bt->boot_time);

    kprintf("  FB         : %llux%llu  %u bpp\n",
            (unsigned long long)fb->width,
            (unsigned long long)fb->height, fb->bpp);

    char vendor[13], brand[49];
    cpu_vendor(vendor); cpu_brand(brand);
    kprintf("  CPU        : %s  (%s)\n", vendor, brand);
    kprintf("  x2APIC     : %s\n", cpu_has_x2apic() ? "supported" : "no");
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
    kprintf("\n");

    fb_statusbar_set("Quanta OS v" QUANTA_VERSION "  |  Booting...");
    fb_statusbar_refresh();

    // 4. ACPI
    extern uint64_t hhdm_off_early;
    hhdm_off_early = limine_hhdm()->offset;
    acpi_init(limine_rsdp()->address);
    fb_boot_step("ACPI", "tables parsed", 0);

    // 5. GDT + IDT
    gdt_init();
    fb_boot_step("GDT", NULL, 0);
    idt_init();
    fb_boot_step("IDT", "256 gates installed", 0);

    // 6. PMM
    pmm_init(limine_hhdm()->offset);
    fb_boot_step("PMM", "bitmap allocator ready", 0);

    // 7. VMM
    vmm_init();
    fb_boot_step("VMM", "PML4 + NX/SCE enabled", 0);

    // 8. Heap
    heap_init();
    fb_boot_step("HEAP", "9 slab caches ready", 0);

    // 9. APIC
    apic_init();
    fb_boot_step("APIC", apic_x2apic_mode() ? "x2APIC mode" : "xAPIC mode", 0);

    // 9b. I/O APIC
    ioapic_init();
    fb_boot_step("IOAPIC",
                 ioapic_available() ? "ready" : "not found",
                 ioapic_available() ? 0 : 1);

    // 10. SMP
    smp_bsp_early_init();
    smp_init();
    fb_boot_step("SMP", "all CPUs online", 0);

    // 11. APIC timer (PIT-calibrated in Phase 3)
    irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
    apic_timer_init(1);
    fb_boot_step("APIC-TIMER", "1 ms  (PIT-calibrated)", 0);

    // 12. Scheduler
    sched_init();
    fb_boot_step("SCHED", "idle tasks ready", 0);

    // 13. VFS (Phase 3: timestamps, permissions, mkdir/unlink/rename)
    vfs_init();
    {
        int fd = vfs_open("/tmp/welcome.txt", O_WRONLY | O_CREAT);
        if (fd >= 0) {
            const char *msg =
                "Welcome to Quanta OS v" QUANTA_VERSION "!\n"
                "\n"
                "New in Phase 3:\n"
                "  mkdir / rm / mv / cp / touch / chmod\n"
                "  ls -l  (inode metadata, timestamps, permissions)\n"
                "  top    (all tasks with CPU affinity)\n"
                "  free   (visual RAM usage bar)\n"
                "  hexdump <file>\n"
                "  kv set/get/del/list  (persistent key-value store)\n"
                "  sleep now shows actual elapsed time\n"
                "\n"
                "Type 'help' for all commands.\n"
                "Type 'ai <topic>' to ask the QAI assistant.\n";
            vfs_write(fd, msg, strlen(msg));
            vfs_close(fd);
        }
        // Create a sample persistent note
        vfs_mkdir("/home/user", VFS_MODE_DIR);
    }
    fb_boot_step("VFS", "QuantaFS + devfs mounted", 0);

    // 14. VirtIO
    virtio_init();
    fb_boot_step("VirtIO", "PCIe scan complete", 0);

    // 15. KV persistence store (Phase 3 — NEW)
    kv_init();
    fb_boot_step("KV-Store",
                 kv_ready() ? "sector 2048 on virtio disk" : "no disk",
                 kv_ready() ? 0 : 1);

    // 16. Keyboard
    keyboard_init();
    fb_boot_step("KBD", "PS/2 IRQ1 via IOAPIC", 0);

    // 17. Enable interrupts
    __asm__ volatile ("sti");
    fb_boot_step("IRQ", "interrupts enabled", 0);

    // ── Ready banner ──────────────────────────────────────────────────────
    kprintf("\n");
    fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
    fb_set_color(0x44EE88, FB_COLOR_BLACK);
    kprintf("  Quanta OS initialised successfully.\n");
    fb_set_color(0x778899, FB_COLOR_BLACK);

    uint32_t ncpus = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
    if (!ncpus) ncpus = 1;
    kprintf("  %u CPU(s)  |  %s  |  QuantaFS  |  %s\n",
            ncpus,
            apic_x2apic_mode() ? "x2APIC" : "xAPIC",
            kv_ready() ? "KV-Store ready" : "KV-Store offline");
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
    kprintf("\n");

    fb_statusbar_set("Quanta OS v" QUANTA_VERSION
                     "  |  help  top  free  ls -l  kv  hexdump  ai");
    fb_statusbar_refresh();

    // 18. Shell task
    task_t *shell_task = task_create("qai-shell", shell_run, NULL, 64 * 1024);
    if (!shell_task) kpanic("[INIT] Cannot create shell task\n");
    sched_add(shell_task);

    cpu_local()->preempt_cnt = 0;
    sched_yield();

    for (;;) __asm__ volatile ("hlt");
}
