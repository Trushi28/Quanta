// ============================================================
//  kmain.c — Quanta OS Kernel Entry Point  v2.1
//
//  Foundation fixes:
//    • ASCII-only banner (renders perfectly on framebuffer)
//    • All boot messages use pure ASCII
//    • Progress indicator during 18-step init
//    • No Unicode box-drawing characters anywhere
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

// ── Boot banner (ASCII only — safe for 8x16 font) ─────────────────────────
static void print_banner(void) {
  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n"
          "   ____                    _       __  ____   _____ \n"
          "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
          " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
          "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
          "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("  Quanta OS  v%s  (%s)\n"
          "  x2APIC  |  SMP  |  VirtIO  |  VFS  |  QAI Shell\n",
          QUANTA_VERSION, QUANTA_ARCH);
}

static void print_div(char c, int w) {
  for (int i = 0; i < w; i++) kprintf("%c", c);
  kprintf("\n");
}
static void print_hdiv(void) { print_div('=', 68); }
static void print_ldiv(void) { print_div('-', 68); }

static const char *mm_type_str(uint64_t t) {
  switch (t) {
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
    uint32_t color = (e->type == LIMINE_MEMMAP_USABLE) ? 0x00FF88 : FB_COLOR_GRAY;
    fb_set_color(color, FB_COLOR_BLACK);
    kprintf("  0x%016llx  0x%016llx  %s\n",
            (unsigned long long)e->base,
            (unsigned long long)e->length, mm_type_str(e->type));
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
}

static void apic_timer_irq(registers_t *r) {
  (void)r;
  sched_tick();
}

// ── kmain ─────────────────────────────────────────────────────────────────
__attribute__((noreturn)) void kmain(void) {

  // 1. Serial
  serial_init();
  serial_write_str("\r\n[Quanta] serial up\r\n");

  // 2. Limine request verification
  if (limine_verify_requests() != 0) {
    serial_write_str("[PANIC] Limine requests not fulfilled\r\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
  }

  // 3. Framebuffer
  struct limine_framebuffer *fb = limine_framebuffers()->framebuffers[0];
  fb_init(fb);
  print_banner();
  print_hdiv();
  fb_set_color(FB_COLOR_GRAY, FB_COLOR_BLACK);

  if (limine_bootloader_info()) {
    kprintf("  Bootloader : %s %s\n", limine_bootloader_info()->name,
            limine_bootloader_info()->version);
  }
  struct limine_kernel_address_response *ka = limine_kernel_addr();
  if (ka) {
    kprintf("  Kernel     : phys 0x%llx  virt 0x%llx\n",
            (unsigned long long)ka->physical_base,
            (unsigned long long)ka->virtual_base);
  }
  struct limine_boot_time_response *bt = limine_boot_time();
  if (bt)
    kprintf("  Boot time  : %lld (Unix)\n", (long long)bt->boot_time);

  kprintf("  Framebuffer: %llux%llu  %u bpp  pitch %llu\n",
          (unsigned long long)fb->width, (unsigned long long)fb->height,
          fb->bpp, (unsigned long long)fb->pitch);
  kprintf("  HHDM offset: 0x%llx\n", (unsigned long long)limine_hhdm()->offset);

  char vendor[13], brand[49];
  cpu_vendor(vendor);
  cpu_brand(brand);
  kprintf("  CPU        : %s  (%s)\n", vendor, brand);
  kprintf("  x2APIC     : %s\n", cpu_has_x2apic() ? "supported" : "no");

  print_ldiv();
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

  // 4. ACPI
  extern uint64_t hhdm_off_early;
  hhdm_off_early = limine_hhdm()->offset;
  kprintf("[ACPI] Parsing tables...\n");
  acpi_init(limine_rsdp()->address);

  // 5. GDT + IDT
  kprintf("[GDT] Initialising...\n");
  gdt_init();
  kprintf("[IDT] Initialising...\n");
  idt_init();

  // 6. PMM
  kprintf("[PMM] Initialising...\n");
  pmm_init(limine_hhdm()->offset);
  kprintf("[MEM] Physical memory map:\n");
  print_memmap();
  print_ldiv();
  pmm_stats();

  // 7. VMM
  kprintf("[VMM] Initialising...\n");
  vmm_init();

  // 8. Heap
  kprintf("[HEAP] Initialising slab allocator...\n");
  heap_init();

  // 9. APIC (local)
  kprintf("[APIC] Initialising local APIC (BSP)...\n");
  apic_init();
  kprintf("[APIC] Mode: %s  LAPIC-ID: %u\n",
          apic_x2apic_mode() ? "x2APIC" : "xAPIC", apic_id());

  // 9b. I/O APIC
  kprintf("[IOAPIC] Initialising...\n");
  ioapic_init();

  // 10. SMP
  kprintf("[SMP] Initialising...\n");
  smp_bsp_early_init();
  smp_init();

  // 11. APIC Timer
  kprintf("[APIC] Arming timer (1 ms periodic)...\n");
  irq_register_handler(APIC_TIMER_VECTOR - IRQ_BASE, apic_timer_irq);
  apic_timer_init(1);

  // 12. Scheduler
  kprintf("[SCHED] Initialising...\n");
  sched_init();

  // 13. VFS
  kprintf("[VFS] Initialising...\n");
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

  // 14. VirtIO
  kprintf("[VirtIO] Scanning PCIe bus...\n");
  virtio_init();

  // 15. Keyboard
  kprintf("[KBD] Initialising PS/2 keyboard...\n");
  keyboard_init();

  // 16. Enable interrupts
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

  // 17. Launch QAI Shell task
  task_t *shell_task = task_create("qai-shell", shell_run, NULL, 64 * 1024);
  if (!shell_task)
    kpanic("[INIT] Cannot create shell task\n");
  sched_add(shell_task);

  cpu_local()->preempt_cnt = 0;
  sched_yield();

  // 18. Idle loop
  for (;;) __asm__ volatile("hlt");
}
