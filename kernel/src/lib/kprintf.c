// ============================================================
//  lib/kprintf.c — Kernel printf (serial + framebuffer)
//
//  Fixed in Phase 2.1: added a global spinlock so that SMP
//  boot messages from multiple APs don't interleave.
// ============================================================
#include "kprintf.h"
#include "../drivers/framebuffer.h"
#include "../drivers/serial.h"
#include "spinlock.h"
#include "string.h"
#include <stdint.h>

// One global lock protects both the serial port and the framebuffer.
// We use the IRQ-saving variant because kprintf may be called from
// interrupt handlers (e.g. the APIC timer ISR prints during init).
static spinlock_t kprintf_lock = SPINLOCK_INIT;

static void kputc(char c) {
  serial_write_char(c);
  fb_putchar(c);
}
static void kputs(const char *s) {
  while (*s)
    kputc(*s++);
}

int kvprintf(const char *fmt, va_list args) {
  int written = 0;
  char numbuf[32];

  while (*fmt) {
    if (*fmt != '%') {
      kputc(*fmt++);
      written++;
      continue;
    }
    fmt++;

    int width = 0, pad_zero = 0, left_align = 0;
    if (*fmt == '-') {
      left_align = 1;
      fmt++;
    }
    if (*fmt == '0') {
      pad_zero = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }

    int is_64 = 0;
    if (*fmt == 'l') {
      fmt++;
      is_64 = 1;
      if (*fmt == 'l') {
        fmt++;
        is_64 = 2;
      }
    }
    if (*fmt == 'z') {
      fmt++;
      is_64 = 1;
    }

    switch (*fmt) {
    case 'd':
    case 'i': {
      int64_t v = is_64 ? va_arg(args, int64_t) : (int64_t)va_arg(args, int);
      kitoa(v, numbuf, 10);
      int len = (int)strlen(numbuf);
      if (!left_align)
        while (len++ < width) {
          kputc(pad_zero ? '0' : ' ');
          written++;
        }
      kputs(numbuf);
      written += (int)strlen(numbuf);
      if (left_align)
        while (len++ < width) {
          kputc(' ');
          written++;
        }
      break;
    }
    case 'u': {
      uint64_t v =
          is_64 ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, unsigned);
      kuitoa(v, numbuf, 10);
      int len = (int)strlen(numbuf);
      if (!left_align)
        while (len++ < width) {
          kputc(pad_zero ? '0' : ' ');
          written++;
        }
      kputs(numbuf);
      written += (int)strlen(numbuf);
      break;
    }
    case 'x':
    case 'X': {
      uint64_t v =
          is_64 ? va_arg(args, uint64_t) : (uint64_t)va_arg(args, unsigned);
      kuitoa(v, numbuf, 16);
      if (*fmt == 'X')
        for (char *p = numbuf; *p; p++)
          if (*p >= 'a' && *p <= 'f')
            *p -= 32;
      int len = (int)strlen(numbuf);
      if (!left_align)
        while (len++ < width) {
          kputc(pad_zero ? '0' : ' ');
          written++;
        }
      kputs(numbuf);
      written += (int)strlen(numbuf);
      break;
    }
    case 'p': {
      uint64_t v = (uint64_t)(uintptr_t)va_arg(args, void *);
      kputs("0x");
      kuitoa(v, numbuf, 16);
      kputs(numbuf);
      written += 2 + (int)strlen(numbuf);
      break;
    }
    case 's': {
      const char *s = va_arg(args, const char *);
      if (!s)
        s = "(null)";
      int len = (int)strlen(s);
      if (!left_align)
        while (len++ < width) {
          kputc(' ');
          written++;
        }
      kputs(s);
      written += (int)strlen(s);
      break;
    }
    case 'c': {
      char c = (char)va_arg(args, int);
      kputc(c);
      written++;
      break;
    }
    case '%':
      kputc('%');
      written++;
      break;
    default:
      kputc('%');
      kputc(*fmt);
      written += 2;
      break;
    }
    fmt++;
  }
  return written;
}

// ── Public API — all serialised through kprintf_lock ─────────────────────

int kprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  uint64_t rflags = spinlock_irq_acquire(&kprintf_lock);
  int r = kvprintf(fmt, ap);
  spinlock_irq_release(&kprintf_lock, rflags);
  va_end(ap);
  return r;
}

void kpanic(const char *fmt, ...) {
  // Acquire the lock but never release it — we're halting anyway.
  // Use a plain acquire (not IRQ-saving) in case we're already in a
  // double-fault / NMI where flags are unknown.
  spinlock_acquire(&kprintf_lock);

  fb_set_color(0xFF4444, 0x000000);
  // Write directly (bypass the now-held lock) by calling kvprintf
  va_list ap;
  va_start(ap, fmt);
  serial_write_str("\r\n\r\n[KERNEL PANIC] ");
  fb_puts("\n\n[KERNEL PANIC] ");
  kvprintf(fmt, ap);
  va_end(ap);
  serial_write_str("\r\nSystem halted. Please reboot.\r\n");
  fb_puts("\n\nSystem halted. Please reboot.\n");

  __asm__ volatile("cli");
  for (;;)
    __asm__ volatile("hlt");
}
