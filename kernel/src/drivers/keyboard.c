// ============================================================
//  drivers/keyboard.c — PS/2 Keyboard (IRQ1)
//  Scancode Set 1 → ASCII translation + ring buffer
//
//  Changed from Phase 2: uses IOAPIC to route IRQ1, NOT the
//  legacy PIC/LINT0 path which is masked by apic_init().
// ============================================================
#include "keyboard.h"
#include "../cpu/apic.h"
#include "../cpu/ioapic.h"
#include "../cpu/isr.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include <stdbool.h>
#include <stddef.h>

static inline uint8_t inb(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p) : "memory");
  return v;
}
static inline void outb(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p) : "memory");
}

#define KBD_DATA 0x60
#define KBD_STATUS 0x64

// ── Ring buffer ────────────────────────────────────────────────────────────
#define BUF_SIZE 256
static char kbd_buf[BUF_SIZE];
static uint16_t kbd_head = 0, kbd_tail = 0;
static spinlock_t kbd_lock = SPINLOCK_INIT;

static bool buf_empty(void) { return kbd_head == kbd_tail; }
static bool buf_push(char c) {
  uint16_t next = (uint16_t)((kbd_head + 1) % BUF_SIZE);
  if (next == kbd_tail)
    return false;
  kbd_buf[kbd_head] = c;
  kbd_head = next;
  return true;
}
static char buf_pop(void) {
  char c = kbd_buf[kbd_tail];
  kbd_tail = (uint16_t)((kbd_tail + 1) % BUF_SIZE);
  return c;
}

// ── Scancode set 1 → ASCII ─────────────────────────────────────────────────
static const char sc_normal[128] = {
    0,   27,   '1',  '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',
    '=', '\b', '\t', 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    '[', ']',  '\n', 0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
    ';', '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
    '.', '/',  0,    '*', 0,    ' ', 0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,
};
static const char sc_shifted[128] = {
    0,   27,   '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
    '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
    '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static bool shift_held = false;
static bool ctrl_held = false;
static bool extended = false;

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LCTRL 0x1D
#define SC_LALT 0x38
#define SC_UP 0x48
#define SC_DOWN 0x50
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D
#define SC_HOME 0x47
#define SC_END 0x4F
#define SC_RELEASE 0x80

static void kbd_irq_handler(registers_t *r) {
  (void)r;
  uint8_t sc = inb(KBD_DATA);

  if (sc == 0xE0) {
    extended = true;
    return;
  }

  bool released = (sc & SC_RELEASE) != 0;
  uint8_t key = sc & ~SC_RELEASE;

  if (key == SC_LSHIFT || key == SC_RSHIFT) {
    shift_held = !released;
    extended = false;
    return;
  }
  if (key == SC_LCTRL) {
    ctrl_held = !released;
    extended = false;
    return;
  }
  if (key == SC_LALT) {
    extended = false;
    return;
  }
  if (released) {
    extended = false;
    return;
  }

  if (extended) {
    extended = false;
    char seq[3] = {'\033', '[', 0};
    switch (key) {
    case SC_UP:
      seq[2] = 'A';
      break;
    case SC_DOWN:
      seq[2] = 'B';
      break;
    case SC_RIGHT:
      seq[2] = 'C';
      break;
    case SC_LEFT:
      seq[2] = 'D';
      break;
    case SC_HOME:
      seq[2] = 'H';
      break;
    case SC_END:
      seq[2] = 'F';
      break;
    default:
      return;
    }
    uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
    buf_push(seq[0]);
    buf_push(seq[1]);
    buf_push(seq[2]);
    spinlock_irq_release(&kbd_lock, rflags);
    return;
  }

  char c = 0;
  if (key < 128)
    c = shift_held ? sc_shifted[key] : sc_normal[key];
  if (!c)
    return;

  if (ctrl_held && c >= 'a' && c <= 'z')
    c = (char)(c - 'a' + 1);

  uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
  buf_push(c);
  spinlock_irq_release(&kbd_lock, rflags);
}

void keyboard_init(void) {
  // Flush stale bytes from the PS/2 controller
  while (inb(KBD_STATUS) & 0x01)
    inb(KBD_DATA);

  // Register the handler at IRQ1 slot (vector IRQ_BASE+1 = 33)
  irq_register_handler(1, kbd_irq_handler);

  if (ioapic_available()) {
    // ── Modern path: IOAPIC routes IRQ1 → vector 33 on BSP ──────────
    // This completely bypasses the legacy PIC/LINT0 path that
    // apic_init() masks.  No PIC manipulation needed here.
    ioapic_redirect(1, IRQ_BASE + 1, 0);
    kprintf("[KBD] IRQ1 routed via IOAPIC (vector %u)\n", IRQ_BASE + 1);
  } else {
    // ── Fallback: try legacy PIC compatibility mode ──────────────────
    // Requires LINT0 to be configured in ExtINT mode on the LAPIC,
    // which apic_init() currently masks.  This path likely won't
    // deliver interrupts unless you re-enable LINT0.  Kept as a
    // fallback for machines without an IOAPIC.
    uint8_t mask;
    __asm__ volatile("inb $0x21,%0" : "=a"(mask));
    mask &= ~(1 << 1);
    __asm__ volatile("outb %0,$0x21" ::"a"(mask));
    kprintf("[KBD] IOAPIC unavailable — using legacy PIC (interrupts may not "
            "work)\n");
  }
}

char kbd_getchar(void) {
  for (;;) {
    uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
    if (!buf_empty()) {
      char c = buf_pop();
      spinlock_irq_release(&kbd_lock, rflags);
      return c;
    }
    spinlock_irq_release(&kbd_lock, rflags);
    // Halt the CPU until an interrupt fires.
    // If the IOAPIC isn't configured correctly this will spin-hlt forever.
    __asm__ volatile("hlt");
  }
}

char kbd_getchar_noblock(void) {
  uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
  char c = buf_empty() ? 0 : buf_pop();
  spinlock_irq_release(&kbd_lock, rflags);
  return c;
}
