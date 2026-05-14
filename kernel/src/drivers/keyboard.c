// ============================================================
//  drivers/keyboard.c — PS/2 Keyboard (IRQ1)
//
//  v2.5.1 fix in kbd_getchar():
//    Changed sched_yield() → sched_sleep_ms(1).
//
//    sched_yield() was a tight spin: it re-queued the shell and
//    immediately picked it again (since shell is the only task),
//    never letting idle run.  Idle never ran → the suspended timer
//    ISR stack on idle never unwound → apic_eoi() for the timer
//    was delayed indefinitely → keyboard IRQ (lower APIC priority
//    class) remained blocked behind the unacknowledged timer.
//
//    sched_sleep_ms(1) properly removes the shell from the run
//    queue for one 1ms tick.  Idle gets scheduled, the suspended
//    ISR unwinds, everything comes back clean.  Maximum input
//    latency: 1ms (imperceptible to users).
//
//    The root cause (EOI sent after handler) is also fixed in
//    isr.c by moving apic_eoi() before the handler call.
//    Both fixes together make the system robust.
// ============================================================
#include "keyboard.h"
#include "../cpu/apic.h"
#include "../cpu/ioapic.h"
#include "../cpu/isr.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include "../sched/sched.h"
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
    return false; // buffer full — drop
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
    0,    27,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',  '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[',  ']',
    '\n', 0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,    '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,
};
static const char sc_shifted[128] = {
    0,    27,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|',  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,
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
#define SC_PGUP 0x49
#define SC_PGDN 0x51
#define SC_DEL 0x53
#define SC_INS 0x52
#define SC_RELEASE 0x80

// Push a 4-byte VT sequence: ESC [ digit ~
static void push_csi4(char digit) {
  buf_push('\033');
  buf_push('[');
  buf_push(digit);
  buf_push('~');
}

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
    uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
    switch (key) {
    case SC_UP:
      buf_push('\033');
      buf_push('[');
      buf_push('A');
      break;
    case SC_DOWN:
      buf_push('\033');
      buf_push('[');
      buf_push('B');
      break;
    case SC_RIGHT:
      buf_push('\033');
      buf_push('[');
      buf_push('C');
      break;
    case SC_LEFT:
      buf_push('\033');
      buf_push('[');
      buf_push('D');
      break;
    case SC_HOME:
      buf_push('\033');
      buf_push('[');
      buf_push('H');
      break;
    case SC_END:
      buf_push('\033');
      buf_push('[');
      buf_push('F');
      break;
    case SC_PGUP:
      push_csi4('5');
      break;
    case SC_PGDN:
      push_csi4('6');
      break;
    case SC_DEL:
      push_csi4('3');
      break;
    case SC_INS:
      push_csi4('2');
      break;
    default:
      break;
    }
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
  if (ctrl_held && c >= 'A' && c <= 'Z')
    c = (char)(c - 'A' + 1);

  uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
  buf_push(c);
  spinlock_irq_release(&kbd_lock, rflags);
}

// ── keyboard_init ──────────────────────────────────────────────────────────
void keyboard_init(void) {
  while (inb(KBD_STATUS) & 0x01)
    inb(KBD_DATA); // flush stale data

  irq_register_handler(1, kbd_irq_handler);

  if (ioapic_available()) {
    ioapic_redirect(1, IRQ_BASE + 1, 0);
    kprintf("[KBD] IRQ1 routed via IOAPIC (vector %u)\n", IRQ_BASE + 1);
  } else {
    uint8_t mask;
    __asm__ volatile("inb $0x21,%0" : "=a"(mask));
    mask &= ~(1 << 1);
    __asm__ volatile("outb %0,$0x21" ::"a"(mask));
    kprintf("[KBD] IOAPIC unavailable — using legacy PIC\n");
  }
}

// ── kbd_getchar ────────────────────────────────────────────────────────────
// Blocks until a character is available.
// Sleeps for 1ms between checks so idle can run and ISR frames unwind.
char kbd_getchar(void) {
  for (;;) {
    uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
    if (!buf_empty()) {
      char c = buf_pop();
      spinlock_irq_release(&kbd_lock, rflags);
      return c;
    }
    spinlock_irq_release(&kbd_lock, rflags);
    sched_sleep_ms(1); // properly yield for one tick — not a spin
  }
}

// ── kbd_getchar_noblock ────────────────────────────────────────────────────
char kbd_getchar_noblock(void) {
  uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
  char c = buf_empty() ? 0 : buf_pop();
  spinlock_irq_release(&kbd_lock, rflags);
  return c;
}
