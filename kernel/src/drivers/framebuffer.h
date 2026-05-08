#pragma once

#include <stdint.h>
#include <limine.h>

// ---------------------------------------------------------------------------
//  drivers/framebuffer.h  —  Pixel-mode text terminal
//
//  Provides a simple scrolling text terminal drawn directly into the
//  Limine framebuffer using an embedded 8×16 bitmap font.
//
//  Colours are 32-bit ARGB / XRGB (R8 G8 B8, the top byte is ignored).
// ---------------------------------------------------------------------------

// Default palette
#define FB_COLOR_BLACK   0x000000
#define FB_COLOR_WHITE   0xFFFFFF
#define FB_COLOR_GREEN   0x00FF00
#define FB_COLOR_RED     0xFF4444
#define FB_COLOR_YELLOW  0xFFFF00
#define FB_COLOR_CYAN    0x00FFFF
#define FB_COLOR_GRAY    0xAAAAAA
#define FB_COLOR_DKGRAY  0x555555

// Initialise the terminal using the first Limine framebuffer.
// Must be called after limine_verify_requests().
void fb_init(struct limine_framebuffer *fb);

// Change foreground / background colours for subsequent output.
void fb_set_color(uint32_t fg, uint32_t bg);

// Print a single character (handles \n, \t, \b).
void fb_putchar(char c);

// Print a null-terminated string.
void fb_puts(const char *s);

// Clear the entire screen to the current background colour.
void fb_clear(void);

// Draw a single pixel (bounds-checked).
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);

// Query terminal size in character cells.
void fb_get_size(uint32_t *cols, uint32_t *rows);
