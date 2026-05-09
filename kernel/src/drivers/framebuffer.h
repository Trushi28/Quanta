#pragma once

#include <limine.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  drivers/framebuffer.h  —  Pixel-mode text terminal  v2.2
//
//  UI additions in v2.2:
//    • Persistent status bar (bottom row, never scrolled over)
//    • Boot-step helper for coloured [  OK  ] / [ WARN ] / [ FAIL ] lines
//    • fb_draw_hline() for lightweight separator lines
//    • Additional palette entries (navy, teal, orange, lime)
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

// v2.2 additions
#define FB_COLOR_NAVY 0x0D1F35   // status-bar background
#define FB_COLOR_LTBLUE 0xAADDFF // status-bar foreground
#define FB_COLOR_TEAL 0x33DDCC   // status-bar accent
#define FB_COLOR_LIME 0x44EE88   // boot OK green
#define FB_COLOR_ORANGE 0xFF9922 // boot WARN orange
#define FB_COLOR_PURPLE 0xBB88FF // decorative

// ── Core terminal API ─────────────────────────────────────────────────────
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

// ── Status bar (bottom reserved row) ─────────────────────────────────────
//
//  The terminal reserves the last character row for a persistent status bar.
//  Content never scrolls into that row.
//
//  Workflow:
//    fb_statusbar_set("text...");   // update stored text
//    fb_statusbar_refresh();        // repaint the status row
//
//  Call fb_statusbar_refresh() any time the text should be repainted
//  (e.g. before each shell prompt).
void fb_statusbar_set(const char *text);
void fb_statusbar_refresh(void);
int fb_statusbar_enabled(void); // 1 once fb_init() has been called

// ── Decorative helpers ────────────────────────────────────────────────────

// Draw a full-width horizontal rule at the CURRENT cursor row using glyph c.
// Advances cursor to next line after drawing.
void fb_draw_hline(char glyph, uint32_t fg, uint32_t bg);

// ── Boot-step helper ──────────────────────────────────────────────────────
//
//  Prints a single boot-step line:
//
//    [  OK  ]  <component>   (status == 0  → green)
//    [ WARN ]  <component>   (status == 1  → orange)
//    [ FAIL ]  <component>   (status == -1 → red)
//
//  component  : short descriptive name, e.g. "ACPI"
//  detail     : appended in gray after the component, or NULL to omit
void fb_boot_step(const char *component, const char *detail, int status);
