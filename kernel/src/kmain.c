// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.5  (Foundation)
//
//  Boot flow:
//    fb_init()  →  fb_splash_draw()          splash appears on screen
//    pmm_init() →  fb_splash_set_backbuf()   double buffering enabled
//    Each init step → fb_splash_progress()   bar + label updated
//    fb_splash_done()                         TSC-timed fade to black
//    Header panel + ready banner painted on clean terminal
//    Shell task launched
//
//  v2.5 changes vs v2.0:
//    • power_init() called immediately after acpi_init() so FADT
//      reset/shutdown data is available before any driver touches ACPI.
//    • SPLASH_TOTAL bumped to 17 (one extra step for power init).
//    • version.h included for centralised QUANTA_VERSION string.
//    • Status bar text updated to include new commands.
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
#include "cpu/power.h"
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
#include "version.h"

#include <stddef.h>
#include <stdint.h>

// Total splash progress steps — must match the number of
// fb_splash_progress() calls below so the bar reaches 100%.
#define SPLASH_TOTAL 17

// ── APIC timer IRQ → scheduler tick ──────────────────────────────────────
static void apic_timer_irq(registers_t *r) {
  (void)r;
  sched_tick();
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void kmain(void) {

  // ── 1. Serial (must come first — used for early panic output) ─────────
  serial_init();
  serial_write_str("\r\n[Quanta v" QUANTA_VERSION "] serial up\r\n");

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
  // ACPI must come before PMM — only needs phys_to_virt() via hhdm_off_early.
  extern uint64_t hhdm_off_early;
  hhdm_off_early = limine_hhdm()->offset;
  acpi_init(limine_rsdp()->address);
  fb_splash_progress(1, SPLASH_TOTAL, "ACPI tables parsed");

  // ── 5. Power management (parses FADT + DSDT _S5_) ─────────────────────
  // Must be called after acpi_init().  All subsequent reboot/shutdown
  // calls (including from the shell) will use the data gathered here.
  power_init();
  fb_splash_progress(2, SPLASH_TOTAL, "Power management ready (ACPI S5)");

  // ── 6. GDT ────────────────────────────────────────────────────────────
  gdt_init();
  fb_splash_progress(3, SPLASH_TOTAL, "GDT + TSS loaded");

  // ── 7. IDT ────────────────────────────────────────────────────────────
  idt_init();
  fb_splash_progress(4, SPLASH_TOTAL, "IDT installed  (256 gates)");

  // ── 8. PMM ────────────────────────────────────────────────────────────
  pmm_init(limine_hhdm()->offset);
  fb_splash_progress(5, SPLASH_TOTAL, "Physical memory manager ready");

  // Enable double buffering now that PMM is up.
  {
    size_t bb_bytes = (size_t)fb->width * fb->height * 4;
    size_t bb_pages = (bb_bytes + 4095) / 4096;
    uint64_t bb_phys = pmm_alloc_n(bb_pages);
    if (bb_phys)
      fb_splash_set_backbuf((uint32_t *)phys_to_virt(bb_phys));
  }

  // ── 9. VMM ────────────────────────────────────────────────────────────
  vmm_init();
  fb_splash_progress(6, SPLASH_TOTAL, "Virtual memory  (PML4, NX+SCE)");

  // ── 10. Heap ──────────────────────────────────────────────────────────
  heap_init();
  fb_splash_progress(7, SPLASH_TOTAL, "Slab allocator  (9 caches)");

  // ── 11. APIC ──────────────────────────────────────────────────────────
  apic_init();
  fb_splash_progress(8, SPLASH_TOTAL,
                     apic_x2apic_mode() ? "x2APIC local APIC"
                                        : "xAPIC local APIC");

  // ── 12. I/O APIC ──────────────────────────────────────────────────────
  ioapic_init();
  fb_splash_progress(9, SPLASH_TOTAL,
                     ioapic_available() ? "I/O APIC ready"
                                        : "I/O APIC not found");

  // ── 13. SMP ───────────────────────────────────────────────────────────
  smp_bsp_early_init();
  smp_init();
  {
    uint32_t n = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
    static char smp_label[32];
    int mi = 0;
    if (n >= 10)
      smp_label[mi++] = '0' + (char)(n / 10);
    smp_label[mi++] = '0' + (char)(n % 10);
    const char *sfx = " CPU(s) online";
    for (const char *p = sfx; *p && mi < 30; p++)
      smp_label[mi++] = *p;
    smp_label[mi] = '\0';
    fb_splash_progress(10, SPLASH_TOTAL, smp_label);
  }

  // ── 14. APIC timer (PIT-calibrated) ───────────────────────────────────
  irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
  apic_timer_init(1);
  fb_splash_progress(11, SPLASH_TOTAL, "APIC timer  1 ms  (PIT-calibrated)");

  // ── 15. Scheduler ─────────────────────────────────────────────────────
  sched_init();
  fb_splash_progress(12, SPLASH_TOTAL, "Scheduler  (round-robin, per-CPU)");

  // ── 16. VFS ───────────────────────────────────────────────────────────
  vfs_init();
  {
    int fd = vfs_open("/tmp/welcome.txt", O_WRONLY | O_CREAT);
    if (fd >= 0) {
      const char *msg =
          "Welcome to Quanta OS v" QUANTA_VERSION " \"" QUANTA_CODENAME "\"!\n"
          "\n"
          "What's new in v2.5 (Foundation Hardening):\n"
          "  shutdown         — ACPI S5 power-off (bare-metal + QEMU)\n"
          "  reboot           — ACPI reset reg → KBC → CF9 → triple-fault\n"
          "  edit <file>      — built-in nano-style text editor\n"
          "  calc <expr>      — infix arithmetic: calc (2+3)*4\n"
          "  grep [-i] <pat>  — search files with highlighting\n"
          "  wc <file>        — count lines / words / bytes\n"
          "  which <cmd>      — check if a command exists\n"
          "  uname            — OS + arch summary\n"
          "  motd             — show this message again\n"
          "  keyboard fix     — sched_yield() replaces hlt in getchar\n"
          "  PageUp/Down/Del  — new keys supported in editor\n"
          "\n"
          "Type 'help' for all commands.  Type 'ai <topic>' to ask QAI.\n";
      vfs_write(fd, msg, strlen(msg));
      vfs_close(fd);
    }
    vfs_mkdir("/home/user", VFS_MODE_DIR);
  }
  fb_splash_progress(13, SPLASH_TOTAL, "QuantaFS + devfs mounted");

  // ── 17. VirtIO ────────────────────────────────────────────────────────
  virtio_init();
  fb_splash_progress(14, SPLASH_TOTAL, "VirtIO block device");

  // ── 18. KV persistent store ───────────────────────────────────────────
  kv_init();
  fb_splash_progress(15, SPLASH_TOTAL,
                     kv_ready() ? "KV-Store  (sector 2048 ready)"
                                : "KV-Store  (no disk)");

  // ── 19. Keyboard ──────────────────────────────────────────────────────
  keyboard_init();
  fb_splash_progress(16, SPLASH_TOTAL, "PS/2 keyboard  (IOAPIC IRQ1)");

  // ── 20. Enable interrupts ─────────────────────────────────────────────
  __asm__ volatile("sti");
  fb_splash_progress(SPLASH_TOTAL, SPLASH_TOTAL, "System ready");

  // ── Splash fade-out ───────────────────────────────────────────────────
  fb_splash_done();

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
  kprintf("  Quanta OS  v%s  \"%s\"  (%s)\n", QUANTA_VERSION, QUANTA_CODENAME,
          QUANTA_ARCH);

  fb_draw_hline((char)0xCD, 0x335577, 0x000000);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

  // Info block
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

  // ── Ready banner ──────────────────────────────────────────────────────
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  fb_set_color(0x44EE88, FB_COLOR_BLACK);
  kprintf("  Quanta OS v%s initialised successfully.\n", QUANTA_VERSION);
  fb_set_color(0x778899, FB_COLOR_BLACK);

  uint32_t ncpus = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
  if (!ncpus)
    ncpus = 1;
  kprintf("  %u CPU(s)  |  %s  |  QuantaFS  |  %s  |  ACPI power\n", ncpus,
          apic_x2apic_mode() ? "x2APIC" : "xAPIC",
          kv_ready() ? "KV ready" : "KV offline");

  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  kprintf("\n");

  // ── Status bar ────────────────────────────────────────────────────────
  fb_statusbar_set("Quanta OS v" QUANTA_VERSION
                   "  |  help  edit  calc  grep  wc  shutdown  ai");
  fb_statusbar_refresh();

  // ── Shell task ────────────────────────────────────────────────────────
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
