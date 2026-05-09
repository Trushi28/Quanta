// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.2
//
//  UI upgrades in v2.2:
//    • fb_boot_step() replaces raw kprintf for every init stage
//    • Header panel with version, arch, CPU count
//    • Status bar seeded before shell starts
//    • Section dividers use fb_draw_hline()
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

// ── Helpers ────────────────────────────────────────────────────────────────
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

static void print_memmap(void) {
  struct limine_memmap_response *mm = limine_memmap();
  kprintf("  %-20s  %-18s  %s\n", "Base", "Length", "Type");
  for (uint64_t i = 0; i < mm->entry_count; i++) {
    struct limine_memmap_entry *e = mm->entries[i];
    uint32_t color = (e->type == LIMINE_MEMMAP_USABLE) ? 0x44EE88 : 0x778899;
    fb_set_color(color, FB_COLOR_BLACK);
    kprintf("  0x%016llx  0x%016llx  %s\n", (unsigned long long)e->base,
            (unsigned long long)e->length, mm_type_str(e->type));
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
}

// ── APIC timer IRQ → scheduler tick ───────────────────────────────────────
static void apic_timer_irq(registers_t *r) {
  (void)r;
  sched_tick();
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void kmain(void) {

  // 1. Serial (before framebuffer)
  serial_init();
  serial_write_str("\r\n[Quanta] serial up\r\n");

  // 2. Limine request verification
  if (limine_verify_requests() != 0) {
    serial_write_str("[PANIC] Limine requests not fulfilled\r\n");
    __asm__ volatile("cli");
    for (;;)
      __asm__ volatile("hlt");
  }

  // 3. Framebuffer + header panel
  struct limine_framebuffer *fb = limine_framebuffers()->framebuffers[0];
  fb_init(fb);

  // ── Header panel ──────────────────────────────────────────────────────
  fb_set_color(FB_COLOR_BLACK, 0x0D1F35); // navy fill
  for (uint32_t c = 0; c < 80; c++)
    fb_putchar(' '); // top padding row

  fb_set_color(0x00D4FF, 0x0D1F35);
  kprintf("   ____                    _       __  ____   _____ \n"
          "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
          " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
          "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
          "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n");

  fb_set_color(0xAADDFF, 0x0D1F35);
  kprintf("  Quanta OS  v%s  (%s)   x2APIC | SMP | VirtIO | VFS | QAI\n",
          QUANTA_VERSION, QUANTA_ARCH);

  // Separator under header (drawn in HLINE style)
  fb_draw_hline((char)0xCD, 0x335577, 0x000000);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

  // Bootloader / kernel info block
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

  char vendor[13], brand[49];
  cpu_vendor(vendor);
  cpu_brand(brand);
  kprintf("  CPU        : %s  (%s)\n", vendor, brand);
  kprintf("  x2APIC     : %s\n", cpu_has_x2apic() ? "supported" : "no");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

  // Thin separator before init steps
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  kprintf("\n");

  // ── Seed status bar ───────────────────────────────────────────────────
  fb_statusbar_set("Quanta OS v" QUANTA_VERSION "  |  Booting...");
  fb_statusbar_refresh();

  // ── Init steps with [  OK  ] / [ FAIL ] badges ────────────────────────

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

  // Optional memory map (collapsed behind a separator — readable on serial)
  serial_write_str("[MEM] Physical memory map:\r\n");

  // 7. VMM
  vmm_init();
  fb_boot_step("VMM", "PML4 + NX/SCE enabled", 0);

  // 8. Heap
  heap_init();
  fb_boot_step("HEAP", "9 slab caches ready", 0);

  // 9. APIC
  apic_init();
  {
    char detail[32];
    const char *mode = apic_x2apic_mode() ? "x2APIC" : "xAPIC";
    /* build detail: mode + LAPIC id */
    size_t i = 0;
    for (const char *p = mode; *p && i < 28; p++, i++)
      detail[i] = *p;
    detail[i++] = ' ';
    detail[i++] = 'L';
    detail[i++] = 'A';
    detail[i++] = 'P';
    detail[i++] = 'I';
    detail[i++] = 'C';
    detail[i++] = '-';
    detail[i++] = 'I';
    detail[i++] = 'D';
    detail[i++] = '=';
    detail[i++] = '0' + (char)(apic_id() % 10);
    detail[i] = '\0';
    fb_boot_step("APIC", detail, 0);
  }

  // 9b. I/O APIC
  ioapic_init();
  fb_boot_step("IOAPIC", ioapic_available() ? "ready" : "not found",
               ioapic_available() ? 0 : 1);

  // 10. SMP
  smp_bsp_early_init();
  smp_init();
  {
    uint32_t ncpus = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
    static char smp_det[32];
    /* simple itoa for cpu count */
    smp_det[0] = '0' + (char)(ncpus % 10);
    smp_det[1] = ' ';
    smp_det[2] = 'C';
    smp_det[3] = 'P';
    smp_det[4] = 'U';
    smp_det[5] = '(';
    smp_det[6] = 's';
    smp_det[7] = ')';
    smp_det[8] = ' ';
    smp_det[9] = 'o';
    smp_det[10] = 'n';
    smp_det[11] = 'l';
    smp_det[12] = 'i';
    smp_det[13] = 'n';
    smp_det[14] = 'e';
    smp_det[15] = '\0';
    fb_boot_step("SMP", smp_det, 0);
  }

  // 11. APIC timer
  irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
  apic_timer_init(1);
  fb_boot_step("APIC-TIMER", "1 ms periodic", 0);

  // 12. Scheduler
  sched_init();
  fb_boot_step("SCHED", "idle tasks ready", 0);

  // 13. VFS
  vfs_init();
  {
    int fd = vfs_open("/tmp/welcome.txt", O_WRONLY | O_CREAT);
    if (fd >= 0) {
      const char *msg = "Welcome to Quanta OS!\n"
                        "Type 'help' for available commands.\n"
                        "Type 'ai <question>' to ask the QAI assistant.\n";
      vfs_write(fd, msg, strlen(msg));
      vfs_close(fd);
    }
  }
  fb_boot_step("VFS", "ramfs + devfs mounted", 0);

  // 14. VirtIO
  virtio_init();
  fb_boot_step("VirtIO", "PCIe scan complete", 0);

  // 15. Keyboard
  keyboard_init();
  fb_boot_step("KBD", "PS/2 IRQ1 via IOAPIC", 0);

  // 16. Enable interrupts
  __asm__ volatile("sti");
  fb_boot_step("IRQ", "interrupts enabled", 0);

  // ── Ready banner ──────────────────────────────────────────────────────
  kprintf("\n");
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  fb_set_color(0x44EE88, FB_COLOR_BLACK);
  kprintf("  Quanta OS initialised successfully.\n");
  fb_set_color(0x778899, FB_COLOR_BLACK);

  uint32_t ncpus = (uint32_t)__atomic_load_n(&g_cpu_count, __ATOMIC_SEQ_CST);
  if (!ncpus)
    ncpus = 1;
  kprintf("  %u CPU(s) online  |  %s  |  VFS ready\n", ncpus,
          apic_x2apic_mode() ? "x2APIC" : "xAPIC");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  fb_draw_hline((char)0xCD, 0x335577, FB_COLOR_BLACK);
  kprintf("\n");

  // ── Update status bar for shell ───────────────────────────────────────
  fb_statusbar_set("Quanta OS v" QUANTA_VERSION
                   "  |  type 'help'  |  'ai <question>' for QAI");
  fb_statusbar_refresh();

  // 17. Launch QAI shell task
  task_t *shell_task = task_create("qai-shell", shell_run, NULL, 64 * 1024);
  if (!shell_task)
    kpanic("[INIT] Cannot create shell task\n");
  sched_add(shell_task);

  cpu_local()->preempt_cnt = 0;
  sched_yield();

  // 18. Idle
  for (;;)
    __asm__ volatile("hlt");
}
