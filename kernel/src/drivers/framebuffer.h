#pragma once

#include <limine.h>
#include <stdint.h>

// ── Colour palette ────────────────────────────────────────────────────────
#define FB_COLOR_BLACK 0x000000
#define FB_COLOR_WHITE 0xFFFFFF
#define FB_COLOR_GREEN 0x00FF00
#define FB_COLOR_RED 0xFF4444
#define FB_COLOR_YELLOW 0xFFFF00
#define FB_COLOR_CYAN 0x00FFFF
#define FB_COLOR_GRAY 0xAAAAAA
#define FB_COLOR_DKGRAY 0x555555
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

// Move terminal cursor without drawing
void fb_set_cursor(uint32_t x, uint32_t y);

// Write a string at absolute (col, row) bypassing the cursor — does NOT
// move the terminal cursor, safe to call during splash.
void fb_print_at(uint32_t col, uint32_t row, const char *s, uint32_t fg,
                 uint32_t bg);

// ── Status bar ────────────────────────────────────────────────────────────
void fb_statusbar_set(const char *text);
void fb_statusbar_refresh(void);
int fb_statusbar_enabled(void);

// ── Decorative helpers ────────────────────────────────────────────────────
void fb_draw_hline(char glyph, uint32_t fg, uint32_t bg);
void fb_boot_step(const char *component, const char *detail, int status);

// ── Boot splash ───────────────────────────────────────────────────────────

// Step 1 — call immediately after fb_init().
// Draws the full-screen splash directly to the real framebuffer
// (PMM does not exist yet so there is no back buffer).
// Sets splash_active = 1 so fb_putchar / fb_draw_hline are no-ops.
void fb_splash_draw(const char *version, const char *arch);

// Step 2 — call right after pmm_init() with a PMM-allocated buffer.
// buf must point to at least (width * height * 4) bytes of mapped RAM.
// From this point all splash drawing goes to the back buffer and
// fb_splash_progress() blits it to screen with a single rep movsl.
//
// Suggested allocation in kmain (after pmm_init):
//   size_t bb_pages = (fb->width * fb->height * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
//   uint64_t bb_phys = pmm_alloc_n(bb_pages);
//   fb_splash_set_backbuf((uint32_t *)phys_to_virt(bb_phys));
void fb_splash_set_backbuf(uint32_t *buf);

// Step 3 — update the progress bar after each init step.
//   step  : 1-based index of the completed step
//   total : total number of steps
//   label : short description shown below the bar
// Calls fb_flip() internally when a back buffer is available.
void fb_splash_progress(int step, int total, const char *label);

// Step 4 — TSC-timed fade to black, then resumes normal terminal output.
// splash_active is cleared; back-buffer reference is dropped (caller is
// responsible for freeing the PMM pages if desired).
void fb_splash_done(void);
