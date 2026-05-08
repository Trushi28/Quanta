#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
//  drivers/serial.h  —  COM1 serial output (great for QEMU -serial stdio)
//
//  Baud rate: 38400
//  Format:    8N1 (8 data bits, no parity, 1 stop bit)
// ---------------------------------------------------------------------------

void serial_init(void);
void serial_write_char(char c);
void serial_write_str(const char *s);
void serial_write_hex(uint64_t v);
