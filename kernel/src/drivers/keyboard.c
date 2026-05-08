// ============================================================
//  drivers/keyboard.c — PS/2 Keyboard (IRQ1)
//  Scancode Set 1 → ASCII translation + ring buffer
// ============================================================
#include "keyboard.h"
#include "../cpu/isr.h"
#include "../cpu/apic.h"
#include "../lib/spinlock.h"
#include <stddef.h>
#include <stdbool.h>

static inline uint8_t inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p):"memory"); return v;
}
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p):"memory");
}

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

// ── Ring buffer ────────────────────────────────────────────────────────────
#define BUF_SIZE 256
static char      kbd_buf[BUF_SIZE];
static uint16_t  kbd_head = 0, kbd_tail = 0;
static spinlock_t kbd_lock = SPINLOCK_INIT;

static bool buf_empty(void) { return kbd_head == kbd_tail; }
static bool buf_push(char c) {
    uint16_t next = (uint16_t)((kbd_head + 1) % BUF_SIZE);
    if (next == kbd_tail) return false;  // full
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
// Lower index = normal, upper = shifted
static const char sc_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b','\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    0,  '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // F1–F10 area
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const char sc_shifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b','\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Modifier state
static bool shift_held   = false;
static bool ctrl_held    = false;
static bool alt_held     = false;
static bool extended     = false;  // E0 prefix

// Special scancodes
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_LCTRL    0x1D
#define SC_LALT     0x38
#define SC_CAPS     0x3A
#define SC_UP       0x48
#define SC_DOWN     0x50
#define SC_LEFT     0x4B
#define SC_RIGHT    0x4D
#define SC_DELETE   0x53
#define SC_HOME     0x47
#define SC_END      0x4F
#define SC_PGUP     0x49
#define SC_PGDN     0x51
#define SC_RELEASE  0x80   // release bit

static void kbd_irq_handler(registers_t *r) {
    (void)r;
    uint8_t sc = inb(KBD_DATA);

    // Extended key prefix
    if (sc == 0xE0) { extended = true; return; }

    bool released = (sc & SC_RELEASE) != 0;
    uint8_t key   = sc & ~SC_RELEASE;

    // Handle modifiers
    if (key == SC_LSHIFT || key == SC_RSHIFT) { shift_held = !released; extended=false; return; }
    if (key == SC_LCTRL)  { ctrl_held  = !released; extended=false; return; }
    if (key == SC_LALT)   { alt_held   = !released; extended=false; return; }
    if (released) { extended = false; return; }

    // Arrow keys (extended)
    if (extended) {
        extended = false;
        char seq[3] = { '\033', '[', 0 };
        switch (key) {
            case SC_UP:    seq[2]='A'; break;
            case SC_DOWN:  seq[2]='B'; break;
            case SC_RIGHT: seq[2]='C'; break;
            case SC_LEFT:  seq[2]='D'; break;
            case SC_HOME:  seq[2]='H'; break;
            case SC_END:   seq[2]='F'; break;
            default: return;
        }
        uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
        buf_push(seq[0]); buf_push(seq[1]); buf_push(seq[2]);
        spinlock_irq_release(&kbd_lock, rflags);
        return;
    }

    // Normal ASCII translation
    char c = 0;
    if (key < 128) {
        c = shift_held ? sc_shifted[key] : sc_normal[key];
    }
    if (!c) return;

    // Ctrl modifier: produce control characters
    if (ctrl_held && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);

    uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
    buf_push(c);
    spinlock_irq_release(&kbd_lock, rflags);
}

void keyboard_init(void) {
    // Flush any stale bytes
    while (inb(KBD_STATUS) & 0x01) inb(KBD_DATA);

    // Register IRQ1 handler
    irq_register_handler(1, kbd_irq_handler);
    pic_unmask_irq(1);   // unmask IRQ1 on the (remapped, but masked) PIC
    // Note: since we use APIC, we need to route IRQ1 via IOAPIC.
    // For now, re-enable on the 8259 as a compatibility path —
    // QEMU routes PS/2 via the legacy PIC even in APIC mode.
    // Re-enable just IRQ1 on PIC1:
    uint8_t mask; __asm__ volatile("inb $0x21,%0":"=a"(mask));
    mask &= ~(1 << 1);  // unmask IRQ1
    __asm__ volatile("outb %0,$0x21"::"a"(mask));
}

char kbd_getchar(void) {
    while (true) {
        uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
        if (!buf_empty()) {
            char c = buf_pop();
            spinlock_irq_release(&kbd_lock, rflags);
            return c;
        }
        spinlock_irq_release(&kbd_lock, rflags);
        __asm__ volatile ("hlt");  // wait for next interrupt
    }
}

char kbd_getchar_noblock(void) {
    uint64_t rflags = spinlock_irq_acquire(&kbd_lock);
    char c = buf_empty() ? 0 : buf_pop();
    spinlock_irq_release(&kbd_lock, rflags);
    return c;
}
