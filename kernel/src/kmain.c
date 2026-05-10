// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.3  (Phase 3)
//
//  Boot flow:
//    fb_init() → fb_splash_draw()          (full-screen splash appears)
//    Each init step calls fb_splash_progress(step, 16, label)
//    fb_splash_done()                       (fade to black)
//    Normal terminal output begins (header panel, then shell)
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

// Total init steps shown on the splash progress bar (Updated to 16 for Phase 3)
#define SPLASH_TOTAL 16

// ── APIC timer → scheduler ────────────────────────────────────────────────
static void apic_timer_irq(registers_t *r) {
  (void)r;
  sched_tick();
}

// ── Memory map helper (serial only during splash) ─────────────────────────
static const char *mm_type_str(uint64_t t) {
  switch (t) {
  case LIMINE_MEMMAP_USABLE:
    return "Usable";
  case LIMINE_MEMMAP_RESERVED:
    return "Reserved";
  case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    return "ACPI Reclaim";
  case LIMINE_MEMMAP_ACPI_NVS:
    return "ACPI NVS";
  case LIMINE_MEMMAP_BAD_MEMORY:
    return "Bad";
  case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    return "BL Reclaim";
  case LIMINE_MEMMAP_KERNEL_AND_MODULES:
    return "Kernel+Modules";
  case LIMINE_MEMMAP_FRAMEBUFFER:
    return "Framebuffer";
  default:
    return "Unknown";
  }
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void kmain(void) {

  // ── 1. Serial ─────────────────────────────────────────────────────────
  serial_init();
  serial_write_str("\r\n[Quanta] serial up\r\n");

  // ── 2. Limine verification ────────────────────────────────────────────
  if (limine_verify_requests() != 0) {
    serial_write_str("[PANIC] Limine requests not fulfilled\r\n");
    __asm__ volatile("cli");
    for (;;)
      __asm__ volatile("hlt");
  }

  // ── 3. Framebuffer + SPLASH ───────────────────────────────────────────
  struct limine_framebuffer *fb = limine_framebuffers()->framebuffers[0];
  fb_init(fb);

  // Draw full-screen splash immediately
  fb_splash_draw(QUANTA_VERSION, QUANTA_ARCH);

  // ── 4. ACPI ───────────────────────────────────────────────────────────
  extern uint64_t hhdm_off_early;
  hhdm_off_early = limine_hhdm()->offset;
  acpi_init(limine_rsdp()->address);
  fb_splash_progress(1, SPLASH_TOTAL, "Parsing ACPI tables");

  // ── 5. GDT ────────────────────────────────────────────────────────────
  gdt_init();
  fb_splash_progress(2, SPLASH_TOTAL, "Loading GDT + TSS");

  // ── 6. IDT ────────────────────────────────────────────────────────────
  idt_init();
  fb_splash_progress(3, SPLASH_TOTAL, "Installing IDT  (256 gates)");

  // ── 7. PMM ────────────────────────────────────────────────────────────
  pmm_init(limine_hhdm()->offset);
  fb_splash_progress(4, SPLASH_TOTAL, "Physical memory manager ready");

  // Log memmap to serial only
  {
    struct limine_memmap_response *mm = limine_memmap();
    for (uint64_t i = 0; i < mm->entry_count; i++) {
      struct limine_memmap_entry *e = mm->entries[i];
      serial_write_str("  mem: ");
      serial_write_str(mm_type_str(e->type));
      serial_write_str("\r\n");
    }
  }

  // ── 8. VMM ────────────────────────────────────────────────────────────
  vmm_init();
  fb_splash_progress(5, SPLASH_TOTAL, "Virtual memory  (PML4, NX+SCE)");

  // ── 9. Heap ───────────────────────────────────────────────────────────
  heap_init();
  fb_splash_progress(6, SPLASH_TOTAL, "Slab allocator  (9 caches)");

  // ── 10. APIC ──────────────────────────────────────────────────────────
  apic_init();
  fb_splash_progress(7, SPLASH_TOTAL,
                     apic_x2apic_mode() ? "x2APIC local APIC"
                                        : "xAPIC local APIC");

  // ── 11. I/O APIC ──────────────────────────────────────────────────────
  ioapic_init();
  fb_splash_progress(8, SPLASH_TOTAL,
                     ioapic_available() ? "I/O APIC ready"
                                        : "I/O APIC not found");

  // ── 12. SMP ───────────────────────────────────────────────────────────
  smp_bsp_early_init();
  smp_init();
  {
    uint32_t n = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
    static char smp_msg[32];
    int mi = 0;
    if (n >= 10)
      smp_msg[mi++] = '0' + (char)(n / 10);
    smp_msg[mi++] = '0' + (char)(n % 10);
    const char *sfx = " CPU(s) online";
    for (const char *p = sfx; *p && mi < 30; p++)
      smp_msg[mi++] = *p;
    smp_msg[mi] = '\0';
    fb_splash_progress(9, SPLASH_TOTAL, smp_msg);
  }

  // ── 13. APIC timer ────────────────────────────────────────────────────
  irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
  apic_timer_init(1);
  fb_splash_progress(10, SPLASH_TOTAL, "APIC timer  1 ms periodic");

  // ── 14. Scheduler ─────────────────────────────────────────────────────
  sched_init();
  fb_splash_progress(11, SPLASH_TOTAL, "Scheduler  (round-robin, per-CPU)");

  // ── 15. VFS (Phase 3) ─────────────────────────────────────────────────
  vfs_init();
  {
    int fd = vfs_open("/tmp/welcome.txt", O_WRONLY | O_CREAT);
    if (fd >= 0) {
      const char *msg = "Welcome to Quanta OS v" QUANTA_VERSION "!\n"
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
    vfs_mkdir("/home/user", VFS_MODE_DIR);
  }
  fb_splash_progress(12, SPLASH_TOTAL, "VFS  (QuantaFS + devfs mounted)");

  // ── 16. VirtIO ────────────────────────────────────────────────────────
  virtio_init();
  fb_splash_progress(13, SPLASH_TOTAL, "VirtIO block device");

  // ── 17. KV-Store (Phase 3) ────────────────────────────────────────────
  kv_init();
  fb_splash_progress(14, SPLASH_TOTAL,
                     kv_ready() ? "KV-Store  (sector 2048 ready)"
                                : "KV-Store  (no disk)");

  // ── 18. Keyboard ──────────────────────────────────────────────────────
  keyboard_init();
  fb_splash_progress(15, SPLASH_TOTAL, "PS/2 keyboard  (IOAPIC IRQ1)");

  // ── 19. Enable interrupts ─────────────────────────────────────────────
  __asm__ volatile("sti");
  fb_splash_progress(SPLASH_TOTAL, SPLASH_TOTAL, "System ready");

  // ── Splash done — fade to black ───────────────────────────────────────
  fb_splash_done();

  // ── Normal terminal header (now visible) ──────────────────────────────

  // Navy header panel
  fb_set_color(FB_COLOR_BLACK, 0x0D1F35);
  uint32_t cols, rows;
  fb_get_size(&cols, &rows);
  for (uint32_t c = 0; c < cols; c++)
    fb_putchar(' '); // padding row

  fb_set_color(0x00D4FF, 0x0D1F35);
  kprintf("   ____                    _       __  ____   _____ \n"
          "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
          " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
          "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
          "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n");

  fb_set_color(0xAADDFF, 0x0D1F35);
  kprintf(
      "  Quanta OS  v%s  (%s)   x2APIC | SMP | VirtIO | QuantaFS | KV | QAI\n",
      QUANTA_VERSION, QUANTA_ARCH);

  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);

  // Brief info row
  {
    char vendor[13];
    cpu_vendor(vendor);
    uint32_t ncpus = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
    if (!ncpus)
      ncpus = 1;
    fb_set_color(0x778899, FB_COLOR_BLACK);
    kprintf("  %s  |  %u CPU(s)  |  %s  |  QuantaFS  |  %s\n", vendor, ncpus,
            apic_x2apic_mode() ? "x2APIC" : "xAPIC",
            kv_ready() ? "KV-Store ready" : "KV-Store offline");
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  }
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  kprintf("\n");

  // Seed status bar for shell
  fb_statusbar_set("Quanta OS v" QUANTA_VERSION
                   "  |  help  top  free  ls -l  kv  hexdump  ai");
  fb_statusbar_refresh();

  // ── 20. Launch shell task ─────────────────────────────────────────────
  task_t *shell_task = task_create("qai-shell", shell_run, NULL, 64 * 1024);
  if (!shell_task)
    kpanic("[INIT] Cannot create shell task\n");
  sched_add(shell_task);

  cpu_local()->preempt_cnt = 0;
  sched_yield();

  // ── 21. BSP idle ──────────────────────────────────────────────────────
  for (;;)
    __asm__ volatile("hlt");
}
