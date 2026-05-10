// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.3  (Phase 3)
//
//  Boot flow:
//    fb_init()  →  fb_splash_draw()          splash appears on screen
//    pmm_init() →  fb_splash_set_backbuf()   double buffering enabled
//    Each init step → fb_splash_progress()   bar + label updated
//    fb_splash_done()                         TSC-timed fade to black
//    Header panel + ready banner painted on clean terminal
//    Shell task launched
//
//  All Phase 3 additions from the uploaded kmain retained:
//    kv_init(), QuantaFS welcome file, /home/user mkdir,
//    status bar with kv / new command hints.
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

// Total splash progress steps (must match the number of fb_splash_progress
// calls below so the bar reaches 100% on the last step).
#define SPLASH_TOTAL 16

// ── APIC timer IRQ → scheduler tick ──────────────────────────────────────
static void apic_timer_irq(registers_t *r) {
  (void)r;
  sched_tick();
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void kmain(void) {

  // ── 1. Serial (must come first — used for early panic output) ─────────
  serial_init();
  serial_write_str("\r\n[Quanta] serial up\r\n");

  // ── 2. Limine request verification ────────────────────────────────────
  if (limine_verify_requests() != 0) {
    serial_write_str("[PANIC] Limine requests not fulfilled\r\n");
    __asm__ volatile("cli");
    for (;;)
      __asm__ volatile("hlt");
  }

  // ── 3. Framebuffer init + full-screen splash ──────────────────────────
  // fb_putchar() becomes a no-op after fb_splash_draw(); all kprintf
  // output from this point until fb_splash_done() goes to serial only.
  struct limine_framebuffer *fb = limine_framebuffers()->framebuffers[0];
  fb_init(fb);
  fb_splash_draw(QUANTA_VERSION, QUANTA_ARCH);

  // ── 4. ACPI ───────────────────────────────────────────────────────────
  // ACPI must come before PMM because it only needs phys_to_virt() which
  // uses hhdm_off_early (set directly from the Limine HHDM response).
  extern uint64_t hhdm_off_early;
  hhdm_off_early = limine_hhdm()->offset;
  acpi_init(limine_rsdp()->address);
  fb_splash_progress(1, SPLASH_TOTAL, "ACPI tables parsed");

  // ── 5. GDT ────────────────────────────────────────────────────────────
  gdt_init();
  fb_splash_progress(2, SPLASH_TOTAL, "GDT + TSS loaded");

  // ── 6. IDT ────────────────────────────────────────────────────────────
  idt_init();
  fb_splash_progress(3, SPLASH_TOTAL, "IDT installed  (256 gates)");

  // ── 7. PMM ────────────────────────────────────────────────────────────
  pmm_init(limine_hhdm()->offset);
  fb_splash_progress(4, SPLASH_TOTAL, "Physical memory manager ready");

  // Enable double buffering now that PMM is up.
  // Allocate enough pages for one full framebuffer frame.
  {
    size_t bb_bytes = (size_t)fb->width * fb->height * 4;
    size_t bb_pages = (bb_bytes + 4095) / 4096;
    uint64_t bb_phys = pmm_alloc_n(bb_pages);
    if (bb_phys)
      fb_splash_set_backbuf((uint32_t *)phys_to_virt(bb_phys));
    // If allocation fails (extremely unlikely at this stage), splash
    // continues without double buffering — no crash.
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
    // Build "N CPU(s) online" without using the heap
    static char smp_label[32];
    int mi = 0;
    if (n >= 10)
      smp_label[mi++] = '0' + (char)(n / 10);
    smp_label[mi++] = '0' + (char)(n % 10);
    const char *sfx = " CPU(s) online";
    for (const char *p = sfx; *p && mi < 30; p++)
      smp_label[mi++] = *p;
    smp_label[mi] = '\0';
    fb_splash_progress(9, SPLASH_TOTAL, smp_label);
  }

  // ── 13. APIC timer (PIT-calibrated, Phase 3) ──────────────────────────
  irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
  apic_timer_init(1);
  fb_splash_progress(10, SPLASH_TOTAL, "APIC timer  1 ms  (PIT-calibrated)");

  // ── 14. Scheduler ─────────────────────────────────────────────────────
  sched_init();
  fb_splash_progress(11, SPLASH_TOTAL, "Scheduler  (round-robin, per-CPU)");

  // ── 15. VFS ───────────────────────────────────────────────────────────
  vfs_init();
  {
    // Write welcome file
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
  fb_splash_progress(12, SPLASH_TOTAL, "QuantaFS + devfs mounted");

  // ── 16. VirtIO ────────────────────────────────────────────────────────
  virtio_init();
  fb_splash_progress(13, SPLASH_TOTAL, "VirtIO block device");

  // ── 17. KV persistent store (Phase 3) ─────────────────────────────────
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

  // ── Splash done — TSC-timed fade to black ─────────────────────────────
  fb_splash_done();
  // From here fb_putchar() is live again; all output appears on screen.

  // ── Header panel ──────────────────────────────────────────────────────
  fb_set_color(FB_COLOR_BLACK, 0x0D1F35);
  {
    uint32_t cols, rows;
    fb_get_size(&cols, &rows);
    (void)rows;
    for (uint32_t c = 0; c < cols; c++)
      fb_putchar(' ');
  }

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

  // Info block (same as uploaded kmain)
  fb_set_color(0x778899, FB_COLOR_BLACK);
  if (limine_bootloader_info())
    kprintf("  Bootloader : %s %s\n", limine_bootloader_info()->name,
            limine_bootloader_info()->version);

  struct limine_kernel_address_response *ka = limine_kernel_addr();
  if (ka)
    kprintf("  Kernel     : phys 0x%llx  virt 0x%llx\n",
            (unsigned long long)ka->physical_base,
            (unsigned long long)ka->virtual_base);

  struct limine_boot_time_response *bt = limine_boot_time();
  if (bt)
    kprintf("  Boot time  : %lld (Unix)\n", (long long)bt->boot_time);

  kprintf("  FB         : %llux%llu  %u bpp\n", (unsigned long long)fb->width,
          (unsigned long long)fb->height, fb->bpp);

  {
    char vendor[13], brand[49];
    cpu_vendor(vendor);
    cpu_brand(brand);
    kprintf("  CPU        : %s  (%s)\n", vendor, brand);
    kprintf("  x2APIC     : %s\n", cpu_has_x2apic() ? "supported" : "no");
    kprintf("  HHDM       : 0x%llx\n",
            (unsigned long long)limine_hhdm()->offset);
  }

  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  kprintf("\n");

  // ── Ready banner (same as uploaded kmain) ──────────────────────────────
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  fb_set_color(0x44EE88, FB_COLOR_BLACK);
  kprintf("  Quanta OS initialised successfully.\n");
  fb_set_color(0x778899, FB_COLOR_BLACK);

  uint32_t ncpus = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
  if (!ncpus)
    ncpus = 1;
  kprintf("  %u CPU(s)  |  %s  |  QuantaFS  |  %s\n", ncpus,
          apic_x2apic_mode() ? "x2APIC" : "xAPIC",
          kv_ready() ? "KV-Store ready" : "KV-Store offline");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  kprintf("\n");

  // ── Status bar (same as uploaded kmain) ───────────────────────────────
  fb_statusbar_set("Quanta OS v" QUANTA_VERSION
                   "  |  help  top  free  ls -l  kv  hexdump  ai");
  fb_statusbar_refresh();

  // ── 20. Shell task ────────────────────────────────────────────────────
  task_t *shell_task = task_create("qai-shell", shell_run, NULL, 64 * 1024);
  if (!shell_task)
    kpanic("[INIT] Cannot create shell task\n");
  sched_add(shell_task);

  cpu_local()->preempt_cnt = 0;
  sched_yield();

  // BSP idle loop
  for (;;)
    __asm__ volatile("hlt");
}
