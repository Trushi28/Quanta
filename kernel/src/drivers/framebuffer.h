#pragma once

#include <limine.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  drivers/framebuffer.h  —  Pixel-mode text terminal  v2.3
//
//  v2.3 additions:
//    • Full-screen boot splash  (navy bg, centred logo, gradient, progress bar)
//    • fb_splash_draw / fb_splash_progress / fb_splash_done
//    • fb_print_at   — write text at absolute (col,row), bypass cursor
//    • fb_set_cursor — move terminal cursor to any position
//    • During splash, fb_putchar / fb_draw_hline are suppressed
//      so kprintf output goes to serial only (clean on-screen splash)
//
//  All v2.2 APIs retained unchanged.
// ---------------------------------------------------------------------------

// ── Colour palette ────────────────────────────────────────────────────────
#define FB_COLOR_BLACK 0x000000
#define FB_COLOR_WHITE 0xFFFFFF
#define FB_COLOR_GREEN 0x00FF00
#define FB_COLOR_RED 0xFF4444
#define FB_COLOR_YELLOW 0xFFFF00
#define FB_COLOR_CYAN 0x00FFFF
#define FB_COLOR_GRAY 0xAAAAAA
#define FB_COLOR_DKGRAY 0x555555
// v2.2+
#define FB_COLOR_NAVY 0x0D1F35
#define FB_COLOR_LTBLUE 0xAADDFF
#define FB_COLOR_TEAL 0x33DDCC
#define FB_COLOR_LIME 0x44EE88
#define FB_COLOR_ORANGE 0xFF9922
#define FB_COLOR_PURPLE 0xBB88FF

// ── Core terminal ─────────────────────────────────────────────────────────
void fb_init(struct limine_framebuffer *fb);
void fb_set_color(uint32_t fg, uint32_t bg);
void fb_putchar(char c);
void fb_puts(const char *s);
void fb_clear(void);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_get_size(uint32_t *cols, uint32_t *rows);
void fb_get_cursor(uint32_t *cx, uint32_t *cy);
void fb_draw_cursor(void);
void fb_erase_cursor(void);

// ── Cursor positioning (v2.3) ─────────────────────────────────────────────
// Move the terminal cursor to (x, y) without drawing anything.
// Useful for side-by-side layouts (e.g. sysinfo command).
void fb_set_cursor(uint32_t x, uint32_t y);

// ── Direct positioned write (v2.3) ───────────────────────────────────────
// Write a string at absolute character cell (col, row) with given fg/bg.
// Does NOT move the terminal cursor.
// ASCII 0x20-0x7E only; extended chars are drawn via their raw CP437 index.
void fb_print_at(uint32_t col, uint32_t row, const char *s, uint32_t fg,
                 uint32_t bg);

// ── Status bar ────────────────────────────────────────────────────────────
void fb_statusbar_set(const char *text);
void fb_statusbar_refresh(void);
int fb_statusbar_enabled(void);

// ── Decorative helpers ────────────────────────────────────────────────────
// Draw a full-width horizontal rule at the current cursor row using glyph c.
// Advances cursor to next line.  Suppressed during splash.
void fb_draw_hline(char glyph, uint32_t fg, uint32_t bg);

// ── Boot step helper ──────────────────────────────────────────────────────
// Prints  [  OK  ] / [ WARN ] / [ FAIL ]  component  detail
// status: 0 = OK, 1 = WARN, -1 = FAIL.
// Suppressed (silent) during splash — call after fb_splash_done().
void fb_boot_step(const char *component, const char *detail, int status);

// ── Boot splash  (v2.3) ───────────────────────────────────────────────────
//
//  Typical usage in kmain:
//
//    fb_init(fb);
//    fb_splash_draw(QUANTA_VERSION, QUANTA_ARCH);
//
//    acpi_init(...);
//    fb_splash_progress(1, 15, "ACPI tables");
//
//    gdt_init();
//    fb_splash_progress(2, 15, "GDT");
//    ...
//    fb_splash_progress(15, 15, "System ready");
//
//    fb_splash_done();   // clears to black, enables normal terminal output
//

// Draw the full-screen splash.  Call immediately after fb_init().
// Sets splash_active = 1 so kprintf/fb_draw_hline are suppressed.
void fb_splash_draw(const char *version, const char *arch);

// Update the progress bar and step label.
//   step  : completed steps so far  (1-based, pass total for 100%)
//   total : total number of steps
//   label : short description of the step just completed (shown below bar)
void fb_splash_progress(int step, int total, const char *label);

// Finish the splash: brief pause, then clear to black and resume
// normal terminal output (splash_active = 0).
void fb_splash_done(void);
