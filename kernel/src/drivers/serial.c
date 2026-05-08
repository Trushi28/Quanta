// ============================================================
//  drivers/serial.c  —  COM1 serial port (UART 8250/16550)
//
//  Used for early debug output (before the framebuffer is up)
//  and as a reliable secondary output alongside the screen.
// ============================================================

#include "serial.h"

// ---------------------------------------------------------------------------
// x86 port I/O helpers
// ---------------------------------------------------------------------------
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

// ---------------------------------------------------------------------------
// UART register offsets (base = COM1 = 0x3F8)
// ---------------------------------------------------------------------------
#define COM1_BASE  0x3F8

#define UART_DATA  (COM1_BASE + 0)   // Data register (R/W)
#define UART_IER   (COM1_BASE + 1)   // Interrupt enable register
#define UART_FCR   (COM1_BASE + 2)   // FIFO control register (write)
#define UART_LCR   (COM1_BASE + 3)   // Line control register
#define UART_MCR   (COM1_BASE + 4)   // Modem control register
#define UART_LSR   (COM1_BASE + 5)   // Line status register
#define UART_DLL   (COM1_BASE + 0)   // Divisor latch low  (DLAB=1)
#define UART_DLH   (COM1_BASE + 1)   // Divisor latch high (DLAB=1)

// LSR bits
#define UART_LSR_THRE  (1 << 5)   // Transmitter hold register empty

// ---------------------------------------------------------------------------
// Initialise COM1 at 38400 baud, 8N1
// ---------------------------------------------------------------------------
void serial_init(void) {
    outb(UART_IER, 0x00);   // Disable all interrupts

    // Enable DLAB to set baud rate divisor
    outb(UART_LCR, 0x80);

    // Divisor for 38400 baud  (115200 / 38400 = 3)
    outb(UART_DLL, 3);
    outb(UART_DLH, 0);

    // 8 data bits, no parity, 1 stop bit (clear DLAB)
    outb(UART_LCR, 0x03);

    // Enable FIFO, clear TX/RX, 14-byte threshold
    outb(UART_FCR, 0xC7);

    // DTR + RTS + OUT2 (enable IRQ line on real hardware)
    outb(UART_MCR, 0x0B);
}

// ---------------------------------------------------------------------------
// Blocking transmit: wait until the holding register is empty
// ---------------------------------------------------------------------------
static void serial_wait_ready(void) {
    while (!(inb(UART_LSR) & UART_LSR_THRE));
}

void serial_write_char(char c) {
    // Translate '\n' → '\r\n' for serial terminals
    if (c == '\n') {
        serial_wait_ready();
        outb(UART_DATA, '\r');
    }
    serial_wait_ready();
    outb(UART_DATA, (uint8_t)c);
}

void serial_write_str(const char *s) {
    while (*s) serial_write_char(*s++);
}

void serial_write_hex(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    serial_write_str("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(hex[(v >> i) & 0xF]);
}
