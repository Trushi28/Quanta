
#include "framebuffer.h"
#include "../lib/string.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Font — 8×16 CP437 bitmap  (identical to v2.2)
// ---------------------------------------------------------------------------
#define FONT_W 8
#define FONT_H 16

// clang-format off
static const uint8_t font_cp437[256][16] = {
    [0x20]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    [0x21]={0,0,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    [0x22]={0,0x66,0x66,0x66,0x24,0,0,0,0,0,0,0,0,0,0,0},
    [0x23]={0,0,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0,0,0,0,0},
    [0x24]={0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0,0},
    [0x25]={0,0,0,0,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0,0,0,0},
    [0x26]={0,0,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    [0x27]={0,0x30,0x30,0x30,0x60,0,0,0,0,0,0,0,0,0,0,0},
    [0x28]={0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0},
    [0x29]={0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0},
    [0x2A]={0,0,0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0},
    [0x2B]={0,0,0,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0,0,0,0,0,0},
    [0x2C]={0,0,0,0,0,0,0,0,0,0,0x18,0x18,0x30,0,0,0},
    [0x2D]={0,0,0,0,0,0,0xFF,0,0,0,0,0,0,0,0,0},
    [0x2E]={0,0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0},
    [0x2F]={0,0,0,0,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0,0,0,0,0},
    [0x30]={0,0,0x38,0x6C,0xC6,0xC6,0xD6,0xD6,0xC6,0xC6,0x6C,0x38,0,0,0,0},
    [0x31]={0,0,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0},
    [0x32]={0,0,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0,0,0,0},
    [0x33]={0,0,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0,0,0,0},
    [0x34]={0,0,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0,0,0,0},
    [0x35]={0,0,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0,0,0,0},
    [0x36]={0,0,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    [0x37]={0,0,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0,0,0,0},
    [0x38]={0,0,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    [0x39]={0,0,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0,0,0,0},
    [0x3A]={0,0,0,0x18,0x18,0,0,0,0x18,0x18,0,0,0,0,0,0},
    [0x3B]={0,0,0,0x18,0x18,0,0,0,0x18,0x18,0x30,0,0,0,0,0},
    [0x3C]={0,0,0,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0,0,0,0},
    [0x3D]={0,0,0,0,0,0xFF,0,0,0xFF,0,0,0,0,0,0,0},
    [0x3E]={0,0,0,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0,0,0,0},
    [0x3F]={0,0,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    [0x40]={0,0,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0xC0,0x7C,0,0,0,0},
    [0x41]={0,0,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    [0x42]={0,0,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0,0,0,0},
    [0x43]={0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0,0,0,0},
    [0x44]={0,0,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0,0,0,0},
    [0x45]={0,0,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0,0,0,0},
    [0x46]={0,0,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0,0,0,0},
    [0x47]={0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0,0,0,0},
    [0x48]={0,0,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    [0x49]={0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    [0x4A]={0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0,0,0,0},
    [0x4B]={0,0,0xE6,0x66,0x6C,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0,0,0,0},
    [0x4C]={0,0,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0,0,0,0},
    [0x4D]={0,0,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    [0x4E]={0,0,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
    [0x4F]={0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    [0x50]={0,0,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0,0,0,0},
    [0x51]={0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0,0},
    [0x52]={0,0,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0,0,0,0},
    [0x53]={0,0,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0,0,0,0},
    [0x54]={0,0,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    [0x55]={0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    [0x56]={0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0},
    [0x57]={0,0,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0xC6,0xC6,0,0,0,0},
    [0x58]={0,0,0xC6,0xC6,0x6C,0x6C,0x38,0x38,0x6C,0x6C,0xC6,0xC6,0,0,0,0},
    [0x59]={0,0,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x30,0x78,0,0,0,0},
    [0x5A]={0,0,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0,0,0,0},
    [0x5B]={0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0},
    [0x5C]={0,0,0,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0,0,0,0,0},
    [0x5D]={0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0},
    [0x5E]={0x10,0x38,0x6C,0xC6,0,0,0,0,0,0,0,0,0,0,0,0},
    [0x5F]={0,0,0,0,0,0,0,0,0,0,0,0xFF,0,0,0,0},
    [0x60]={0,0x30,0x18,0x0C,0,0,0,0,0,0,0,0,0,0,0,0},
    [0x61]={0,0,0,0,0,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    [0x62]={0,0,0xE0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0xDC,0,0,0,0},
    [0x63]={0,0,0,0,0,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0,0,0,0},
    [0x64]={0,0,0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    [0x65]={0,0,0,0,0,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0,0,0,0},
    [0x66]={0,0,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0,0,0,0},
    [0x67]={0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0,0},
    [0x68]={0,0,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0,0,0,0},
    [0x69]={0,0,0x18,0x18,0,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    [0x6A]={0,0,0x06,0x06,0,0x0E,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0,0},
    [0x6B]={0,0,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0,0,0,0},
    [0x6C]={0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
    [0x6D]={0,0,0,0,0,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0,0,0,0},
    [0x6E]={0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0,0,0,0},
    [0x6F]={0,0,0,0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
    [0x70]={0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0,0},
    [0x71]={0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0,0},
    [0x72]={0,0,0,0,0,0xDC,0x76,0x62,0x60,0x60,0x60,0xF0,0,0,0,0},
    [0x73]={0,0,0,0,0,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0,0,0,0},
    [0x74]={0,0,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0,0,0,0},
    [0x75]={0,0,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
    [0x76]={0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0,0,0,0},
    [0x77]={0,0,0,0,0,0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0x6C,0,0,0,0},
    [0x78]={0,0,0,0,0,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0,0,0,0},
    [0x79]={0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0,0},
    [0x7A]={0,0,0,0,0,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0,0,0,0},
    [0x7B]={0,0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0,0,0,0},
    [0x7C]={0,0,0x18,0x18,0x18,0x18,0,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
    [0x7D]={0,0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0,0,0,0},
    [0x7E]={0,0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0},
    [0xB0]={0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA},
    [0xB1]={0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55},
    [0xB2]={0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77,0xDD,0x77},
    [0xB3]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xB4]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xB9]={0x66,0x66,0x66,0x66,0x66,0x66,0xFE,0x60,0xE0,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xBA]={0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xBB]={0,0,0,0,0,0,0xFE,0x60,0xE0,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xBC]={0x66,0x66,0x66,0x66,0x66,0x66,0xFE,0x60,0xE0,0,0,0,0,0,0,0},
    [0xBF]={0,0,0,0,0,0,0,0xF8,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xC0]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0,0,0,0,0,0,0,0},
    [0xC1]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0,0,0,0,0,0,0,0},
    [0xC2]={0,0,0,0,0,0,0,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xC3]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xC4]={0,0,0,0,0,0,0,0xFF,0,0,0,0,0,0,0,0},
    [0xC5]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xC8]={0x66,0x66,0x66,0x66,0x66,0x66,0x7F,0x06,0x07,0,0,0,0,0,0,0},
    [0xC9]={0,0,0,0,0,0,0x7F,0x06,0x07,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xCA]={0x66,0x66,0x66,0x66,0x66,0x66,0xFF,0x66,0xFF,0,0,0,0,0,0,0},
    [0xCB]={0,0,0,0,0,0,0xFF,0x66,0xFF,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xCC]={0x66,0x66,0x66,0x66,0x66,0x66,0x7F,0x06,0x07,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xCD]={0,0,0,0,0,0,0xFF,0,0xFF,0,0,0,0,0,0,0},
    [0xCE]={0x66,0x66,0x66,0x66,0x66,0x66,0xFF,0x66,0xFF,0x66,0x66,0x66,0x66,0x66,0x66,0x66},
    [0xD9]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0xF8,0,0,0,0,0,0,0,0},
    [0xDA]={0,0,0,0,0,0,0,0x1F,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    [0xDB]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    [0xDC]={0,0,0,0,0,0,0,0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    [0xDD]={0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0},
    [0xDE]={0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F},
    [0xDF]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0,0,0,0,0,0,0,0},
    [0xFE]={0,0,0,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0,0,0,0},
};
// clang-format on

// ===========================================================================
// Terminal state
// ===========================================================================
static struct {
  uint32_t *base; // real framebuffer (GOP)
  uint32_t width, height;
  uint32_t pitch; // uint32_t pixels per scanline

  uint32_t cols, rows;
  uint32_t scroll_rows; // rows - 1 (last row = status bar)

  uint32_t cursor_x, cursor_y;
  uint32_t fg, bg;

  // ANSI state machine
  int ansi_state;
  uint32_t ansi_params[8];
  int ansi_pcount;

  // UTF-8 state machine
  int utf8_rem, utf8_expect;
  uint32_t utf8_cp;

  // Status bar
  char status_text[256];
  int sb_enabled;

  // Splash
  int splash_active;

  // Double-buffer for splash (NULL until fb_splash_set_backbuf is called)
  uint32_t *backbuf;

  // Splash layout (set once in fb_splash_draw)
  uint32_t sp_bar_pixel_x;
  uint32_t sp_bar_pixel_y;
  uint32_t sp_bar_w_px;
  uint32_t sp_bar_h_px;
  uint32_t sp_label_row; // character row for step label
} term;

// ===========================================================================
// Fast primitives  (rep stosl / rep movsl)
// ===========================================================================

// Fill `count` uint32_t values at dst with val.
// Uses rep stosl — equivalent to a hand-unrolled memset but in one instruction.
static void fast_fill32(uint32_t *dst, uint32_t val, size_t count) {
  __asm__ volatile("cld\n\t"
                   "rep stosl\n\t"
                   : "+D"(dst), "+c"(count)
                   : "a"(val)
                   : "memory");
}

// Copy `count` uint32_t values from src to dst (non-overlapping).
// Uses rep movsl — equivalent to memcpy but in one instruction.
static void fast_copy32(uint32_t *dst, const uint32_t *src, size_t count) {
  __asm__ volatile("cld\n\t"
                   "rep movsl\n\t"
                   : "+D"(dst), "+S"(src), "+c"(count)
                   :
                   : "memory");
}

// ===========================================================================
// TSC-based timing
// ===========================================================================

static inline uint64_t fb_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// Spin for at least `ms` milliseconds.
// Assumes TSC frequency >= 1 GHz (conservative — all x86_64 CPUs since ~2005).
// At 1 GHz: 1 ms = 1,000,000 ticks.  At 3 GHz it will be 3× shorter than
// requested, but the fade animation still looks good at 3× speed.
// Worst-case overrun: none — we always spin at least the requested time.
static void fb_spin_ms(uint32_t ms) {
  uint64_t start = fb_rdtsc();
  uint64_t target = start + (uint64_t)ms * 1000000ULL;
  while (fb_rdtsc() < target)
    __asm__ volatile("pause");
}

// ===========================================================================
// Pixel / rect helpers
// ===========================================================================

// Choose write target: back buffer if available during splash, else real FB.
static inline uint32_t *splash_target(void) {
  return (term.splash_active && term.backbuf) ? term.backbuf : term.base;
}

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t c) {
  uint32_t *dst = splash_target();
  if (x < term.width && y < term.height)
    dst[y * term.pitch + x] = c;
}

// Fill a rectangle using fast_fill32 (one rep stosl per scanline).
static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t c) {
  uint32_t *dst = splash_target();
  for (uint32_t py = y; py < y + h && py < term.height; py++) {
    uint32_t len = w;
    if (x + len > term.width)
      len = term.width - x;
    fast_fill32(dst + py * term.pitch + x, c, len);
  }
}

// Fill FONT_H pixel rows for one character row.
static void fill_row_pixels(uint32_t char_row, uint32_t color) {
  uint32_t *dst = splash_target();
  // During normal terminal use, always write to term.base.
  if (!term.splash_active)
    dst = term.base;
  fast_fill32(dst + char_row * FONT_H * term.pitch, color,
              (size_t)FONT_H * term.pitch);
}

// ===========================================================================
// Blit back buffer → real framebuffer  (rep movsl)
// ===========================================================================
static void fb_flip(void) {
  if (!term.backbuf)
    return;
  fast_copy32(term.base, term.backbuf, (size_t)term.height * term.pitch);
}

// ===========================================================================
// Scroll (content area only)
// ===========================================================================
static void scroll_up(void) {
  size_t char_row_px = (size_t)FONT_H * term.pitch;
  // Shift rows 1..scroll_rows-1  →  0..scroll_rows-2  (rep movsl)
  fast_copy32(term.base, term.base + char_row_px,
              char_row_px * (term.scroll_rows - 1));
  // Clear the new bottom content row
  fast_fill32(term.base + (term.scroll_rows - 1) * char_row_px, term.bg,
              char_row_px);
}

// ===========================================================================
// Glyph drawing
// ===========================================================================
static void draw_glyph_raw(uint32_t col, uint32_t row, char ch, uint32_t fg,
                           uint32_t bg) {
  uint32_t *dst = splash_target();
  if (!term.splash_active)
    dst = term.base;
  const uint8_t *g = font_cp437[(uint8_t)ch];
  uint32_t bx = col * FONT_W, by = row * FONT_H;
  for (uint32_t y = 0; y < FONT_H; y++) {
    uint8_t bits = g[y];
    for (uint32_t x = 0; x < FONT_W; x++) {
      uint32_t px = bx + x, py = by + y;
      if (px < term.width && py < term.height)
        dst[py * term.pitch + px] = (bits & (0x80u >> x)) ? fg : bg;
    }
  }
}

static void draw_glyph(uint32_t col, uint32_t row, char ch) {
  draw_glyph_raw(col, row, ch, term.fg, term.bg);
}

// ===========================================================================
// UTF-8 → CP437 fallback
// ===========================================================================
static char utf8_map(uint32_t cp) {
  switch (cp) {
  case 0x2014:
  case 0x2013:
  case 0x2010:
  case 0x2011:
    return '-';
  case 0x2500:
  case 0x2501:
    return (char)0xC4;
  case 0x2502:
  case 0x2503:
    return (char)0xB3;
  case 0x250C:
  case 0x250F:
    return (char)0xDA;
  case 0x2510:
  case 0x2513:
    return (char)0xBF;
  case 0x2514:
  case 0x2517:
    return (char)0xC0;
  case 0x2518:
  case 0x251B:
    return (char)0xD9;
  case 0x251C:
  case 0x2523:
    return (char)0xC3;
  case 0x2524:
  case 0x252B:
    return (char)0xB4;
  case 0x252C:
  case 0x2533:
    return (char)0xC2;
  case 0x2534:
  case 0x253B:
    return (char)0xC1;
  case 0x253C:
  case 0x254B:
    return (char)0xC5;
  case 0x2550:
    return (char)0xCD;
  case 0x2551:
    return (char)0xBA;
  case 0x2554:
    return (char)0xC9;
  case 0x2557:
    return (char)0xBB;
  case 0x255A:
    return (char)0xC8;
  case 0x255D:
    return (char)0xBC;
  case 0x2560:
    return (char)0xCC;
  case 0x2563:
    return (char)0xB9;
  case 0x2566:
    return (char)0xCB;
  case 0x2569:
    return (char)0xCA;
  case 0x256C:
    return (char)0xCE;
  case 0x2588:
  case 0x2589:
  case 0x258A:
  case 0x258B:
  case 0x258C:
  case 0x258D:
  case 0x258E:
  case 0x258F:
    return (char)0xDB;
  case 0x2590:
    return (char)0xDE;
  case 0x2584:
    return (char)0xDC;
  case 0x2580:
    return (char)0xDF;
  case 0x2591:
    return (char)0xB0;
  case 0x2592:
    return (char)0xB1;
  case 0x2593:
    return (char)0xB2;
  case 0x2190:
    return '<';
  case 0x2191:
    return '^';
  case 0x2192:
    return '>';
  case 0x2193:
    return 'v';
  case 0x2022:
  case 0x25AA:
    return (char)0xFE;
  case 0x00B7:
    return '.';
  case 0x00A9:
    return 'C';
  default:
    return 0;
  }
}

// ===========================================================================
// ANSI colour
// ===========================================================================
static uint32_t ansi_color_to_hex(uint32_t code) {
  switch (code) {
  case 30:
    return 0x000000;
  case 31:
    return 0xAA0000;
  case 32:
    return 0x00AA00;
  case 33:
    return 0xAA5500;
  case 34:
    return 0x0000AA;
  case 35:
    return 0xAA00AA;
  case 36:
    return 0x00AAAA;
  case 37:
    return 0xAAAAAA;
  case 90:
    return 0x555555;
  case 91:
    return 0xFF5555;
  case 92:
    return 0x55FF55;
  case 93:
    return 0xFFFF55;
  case 94:
    return 0x5555FF;
  case 95:
    return 0xFF55FF;
  case 96:
    return 0x55FFFF;
  case 97:
    return 0xFFFFFF;
  default:
    return term.fg;
  }
}

static void apply_ansi_sgr(void) {
  if (!term.ansi_pcount) {
    term.fg = 0xFFFFFF;
    term.bg = 0;
    return;
  }
  for (int i = 0; i < term.ansi_pcount; i++) {
    uint32_t p = term.ansi_params[i];
    if (p == 0) {
      term.fg = 0xFFFFFF;
      term.bg = 0;
    } else if ((p >= 30 && p <= 37) || (p >= 90 && p <= 97))
      term.fg = ansi_color_to_hex(p);
    else if (p >= 40 && p <= 47)
      term.bg = ansi_color_to_hex(p - 10);
  }
}

// ===========================================================================
// Public API — core
// ===========================================================================

void fb_init(struct limine_framebuffer *fb) {
  term.base = (uint32_t *)fb->address;
  term.width = (uint32_t)fb->width;
  term.height = (uint32_t)fb->height;
  term.pitch = (uint32_t)(fb->pitch / 4);
  term.cols = term.width / FONT_W;
  term.rows = term.height / FONT_H;
  term.scroll_rows = (term.rows > 2) ? term.rows - 1 : term.rows;
  term.cursor_x = term.cursor_y = 0;
  term.fg = FB_COLOR_WHITE;
  term.bg = FB_COLOR_BLACK;
  term.ansi_state = term.ansi_pcount = 0;
  for (int i = 0; i < 8; i++)
    term.ansi_params[i] = 0;
  term.utf8_rem = term.utf8_cp = term.utf8_expect = 0;
  term.status_text[0] = '\0';
  term.sb_enabled = 1;
  term.splash_active = 0;
  term.backbuf = 0;

  fast_fill32(term.base, 0, (size_t)term.height * term.pitch);
  fb_statusbar_refresh();
}

void fb_set_color(uint32_t fg, uint32_t bg) {
  term.fg = fg;
  term.bg = bg;
}

void fb_set_cursor(uint32_t x, uint32_t y) {
  term.cursor_x = (x < term.cols) ? x : term.cols - 1;
  term.cursor_y = (y < term.scroll_rows) ? y : term.scroll_rows - 1;
}

void fb_print_at(uint32_t col, uint32_t row, const char *s, uint32_t fg,
                 uint32_t bg) {
  uint32_t *save_bb = term.backbuf;
  term.backbuf = 0; // print_at always targets term.base
  int save_splash = term.splash_active;
  term.splash_active = 0;
  while (*s && col < term.cols)
    draw_glyph_raw(col++, row, *s++, fg, bg);
  term.backbuf = save_bb;
  term.splash_active = save_splash;
}

void fb_clear(void) {
  fast_fill32(term.base, term.bg,
              (size_t)term.scroll_rows * FONT_H * term.pitch);
  term.cursor_x = term.cursor_y = 0;
  fb_statusbar_refresh();
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  put_pixel(x, y, color);
}

void fb_get_size(uint32_t *cols, uint32_t *rows) {
  if (cols)
    *cols = term.cols;
  if (rows)
    *rows = term.scroll_rows;
}

void fb_get_cursor(uint32_t *cx, uint32_t *cy) {
  if (cx)
    *cx = term.cursor_x;
  if (cy)
    *cy = term.cursor_y;
}

// ---------------------------------------------------------------------------
// fb_putchar — suppressed during splash  (goes to serial via kprintf)
// ---------------------------------------------------------------------------
void fb_putchar(char c) {
  if (!term.base || term.splash_active)
    return;
  uint8_t uc = (uint8_t)c;

  // UTF-8
  if (uc >= 0x80) {
    if (term.utf8_rem == 0) {
      if ((uc & 0xE0) == 0xC0) {
        term.utf8_expect = 2;
        term.utf8_cp = uc & 0x1F;
      } else if ((uc & 0xF0) == 0xE0) {
        term.utf8_expect = 3;
        term.utf8_cp = uc & 0x0F;
      } else if ((uc & 0xF8) == 0xF0) {
        term.utf8_expect = 4;
        term.utf8_cp = uc & 0x07;
      } else {
        fb_putchar('?');
        return;
      }
      term.utf8_rem = term.utf8_expect - 1;
    } else {
      if ((uc & 0xC0) == 0x80) {
        term.utf8_cp = (term.utf8_cp << 6) | (uc & 0x3F);
        if (--term.utf8_rem == 0) {
          char r = utf8_map(term.utf8_cp);
          fb_putchar(r ? r : '?');
        }
      } else {
        term.utf8_rem = 0;
        fb_putchar('?');
      }
    }
    return;
  }

  // ANSI
  if (term.ansi_state == 1) {
    if (c == '[') {
      term.ansi_state = 2;
      term.ansi_pcount = 0;
      term.ansi_params[0] = 0;
    } else
      term.ansi_state = 0;
    return;
  }
  if (term.ansi_state == 2) {
    if (c >= '0' && c <= '9')
      term.ansi_params[term.ansi_pcount] =
          term.ansi_params[term.ansi_pcount] * 10 + (c - '0');
    else if (c == ';') {
      if (term.ansi_pcount < 7) {
        term.ansi_pcount++;
        term.ansi_params[term.ansi_pcount] = 0;
      }
    } else if (c == 'm') {
      term.ansi_pcount++;
      apply_ansi_sgr();
      term.ansi_state = 0;
    } else if (c == 'C') {
      uint32_t n = term.ansi_params[0] ? term.ansi_params[0] : 1;
      term.cursor_x =
          (term.cursor_x + n < term.cols) ? term.cursor_x + n : term.cols - 1;
      term.ansi_state = 0;
    } else if (c == 'D') {
      uint32_t n = term.ansi_params[0] ? term.ansi_params[0] : 1;
      term.cursor_x = (term.cursor_x >= n) ? term.cursor_x - n : 0;
      term.ansi_state = 0;
    } else if (c == 'A') {
      uint32_t n = term.ansi_params[0] ? term.ansi_params[0] : 1;
      term.cursor_y = (term.cursor_y >= n) ? term.cursor_y - n : 0;
      term.ansi_state = 0;
    } else if (c == 'B') {
      uint32_t n = term.ansi_params[0] ? term.ansi_params[0] : 1;
      term.cursor_y = (term.cursor_y + n < term.scroll_rows)
                          ? term.cursor_y + n
                          : term.scroll_rows - 1;
      term.ansi_state = 0;
    } else if (c == 'H' || c == 'f') {
      uint32_t r = term.ansi_params[0] ? term.ansi_params[0] - 1 : 0;
      uint32_t cl = (term.ansi_pcount > 1 && term.ansi_params[1])
                        ? term.ansi_params[1] - 1
                        : 0;
      term.cursor_y = (r < term.scroll_rows) ? r : term.scroll_rows - 1;
      term.cursor_x = (cl < term.cols) ? cl : term.cols - 1;
      term.ansi_state = 0;
    } else if (c == 'J') {
      if (term.ansi_params[0] == 2)
        fb_clear();
      term.ansi_state = 0;
    } else if (c == 'K') {
      for (uint32_t x = term.cursor_x; x < term.cols; x++)
        draw_glyph(x, term.cursor_y, ' ');
      term.ansi_state = 0;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
      term.ansi_state = 0;
    return;
  }
  if (c == '\x1b') {
    term.ansi_state = 1;
    return;
  }

  // Normal character
  switch (c) {
  case '\n':
    term.cursor_x = 0;
    term.cursor_y++;
    break;
  case '\r':
    term.cursor_x = 0;
    break;
  case '\t':
    term.cursor_x = (term.cursor_x + 8) & ~7u;
    break;
  case '\b':
    if (term.cursor_x > 0) {
      term.cursor_x--;
      draw_glyph(term.cursor_x, term.cursor_y, ' ');
    }
    break;
  default:
    if (uc >= 0x20) {
      draw_glyph(term.cursor_x, term.cursor_y, c);
      term.cursor_x++;
    }
    break;
  }
  if (term.cursor_x >= term.cols) {
    term.cursor_x = 0;
    term.cursor_y++;
  }
  while (term.cursor_y >= term.scroll_rows) {
    scroll_up();
    term.cursor_y--;
  }
}

void fb_puts(const char *s) {
  while (*s)
    fb_putchar(*s++);
}

void fb_draw_cursor(void) {
  if (!term.base || term.splash_active)
    return;
  uint32_t bx = term.cursor_x * FONT_W,
           by = term.cursor_y * FONT_H + FONT_H - 3;
  for (uint32_t y = 0; y < 2; y++)
    for (uint32_t x = 0; x < FONT_W; x++)
      if (bx + x < term.width && by + y < term.height)
        term.base[(by + y) * term.pitch + (bx + x)] = term.fg;
}

void fb_erase_cursor(void) {
  if (!term.base || term.splash_active)
    return;
  uint32_t bx = term.cursor_x * FONT_W,
           by = term.cursor_y * FONT_H + FONT_H - 3;
  for (uint32_t y = 0; y < 2; y++)
    for (uint32_t x = 0; x < FONT_W; x++)
      if (bx + x < term.width && by + y < term.height)
        term.base[(by + y) * term.pitch + (bx + x)] = term.bg;
}

// ===========================================================================
// Status bar
// ===========================================================================
#define SB_BG 0x0D1F35
#define SB_FG 0xCCDDEE
#define SB_ACC 0x33DDCC
#define SB_SEP 0x335577

void fb_statusbar_set(const char *text) {
  if (!text) {
    term.status_text[0] = '\0';
    return;
  }
  size_t i = 0;
  while (i < sizeof(term.status_text) - 1 && text[i]) {
    term.status_text[i] = text[i];
    i++;
  }
  term.status_text[i] = '\0';
}

void fb_statusbar_refresh(void) {
  if (!term.base || !term.sb_enabled || term.splash_active)
    return;
  uint32_t row = term.scroll_rows;
  if (row >= term.rows)
    return;

  // Fast fill the entire status row
  fast_fill32(term.base + row * FONT_H * term.pitch, SB_BG,
              (size_t)FONT_H * term.pitch);

  uint32_t col = 1;
  draw_glyph_raw(col++, row, '>', SB_ACC, SB_BG);
  draw_glyph_raw(col++, row, ' ', SB_FG, SB_BG);

  for (const char *p = term.status_text; *p && col < term.cols - 10;
       p++, col++) {
    uint8_t uc = (uint8_t)*p;
    if (uc >= 0x20 && uc < 0x80)
      draw_glyph_raw(col, row, *p, (*p == '|' ? SB_SEP : SB_FG), SB_BG);
  }

  const char *badge = "[READY]";
  uint32_t blen = 0;
  for (const char *p = badge; *p; p++)
    blen++;
  if (term.cols > blen + 2) {
    uint32_t rc = term.cols - blen - 1;
    for (const char *p = badge; *p && rc < term.cols; p++, rc++)
      draw_glyph_raw(rc, row, *p, 0x44EE88, SB_BG);
  }
}

int fb_statusbar_enabled(void) { return term.sb_enabled; }

// ===========================================================================
// Decorative helpers
// ===========================================================================
void fb_draw_hline(char glyph, uint32_t fg, uint32_t bg) {
  if (!term.base || term.splash_active)
    return;
  uint32_t sf = term.fg, sb2 = term.bg;
  term.fg = fg;
  term.bg = bg;
  for (uint32_t x = 0; x < term.cols; x++)
    draw_glyph(x, term.cursor_y, glyph);
  term.cursor_x = 0;
  term.cursor_y++;
  if (term.cursor_y >= term.scroll_rows) {
    scroll_up();
    term.cursor_y--;
  }
  term.fg = sf;
  term.bg = sb2;
}

void fb_boot_step(const char *component, const char *detail, int status) {
  if (term.splash_active)
    return;
  uint32_t sf = term.fg, sb2 = term.bg;
  if (status == 0)
    fb_puts("  [  \033[92mOK\033[0m  ]  ");
  else if (status == 1)
    fb_puts("  [\033[93m WARN \033[0m]  ");
  else
    fb_puts("  [\033[91m FAIL \033[0m]  ");
  term.fg =
      (status == 0) ? FB_COLOR_WHITE : (status == 1 ? 0xFFDD88 : 0xFF8888);
  term.bg = 0;
  fb_puts(component);
  if (detail && *detail) {
    term.fg = 0x778899;
    fb_puts("  ");
    fb_puts(detail);
  }
  term.fg = sf;
  term.bg = sb2;
  fb_putchar('\n');
}

// ===========================================================================
// Boot Splash  (v2.3)
// ===========================================================================

// Provide PMM-allocated back buffer after pmm_init().
// Call: fb_splash_set_backbuf((uint32_t*)phys_to_virt(pmm_alloc_n(pages)));
// where pages = (width * height * 4 + PAGE_SIZE - 1) / PAGE_SIZE
void fb_splash_set_backbuf(uint32_t *buf) {
  term.backbuf = buf;
  if (buf) {
    // Copy current framebuffer to back buffer so early steps
    // (drawn before back buffer existed) are preserved
    fast_copy32(buf, term.base, (size_t)term.height * term.pitch);
  }
}

// ---------- Pseudo-random star field ----------
static uint32_t lcg_state = 0xDEADBEEF;
static uint32_t lcg_next(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return lcg_state;
}

// Linear interpolate two RGB colours
static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t t, uint32_t tmax) {
  if (!tmax)
    tmax = 1;
  uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab2 = a & 0xFF;
  uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb2 = b & 0xFF;
  return ((ar + (br - ar) * t / tmax) << 16) |
         ((ag + (bg - ag) * t / tmax) << 8) | (ab2 + (bb2 - ab2) * t / tmax);
}

static void splash_stars(void) {
  lcg_state = 0xDEADBEEF;
  for (int i = 0; i < 300; i++) {
    uint32_t sx = lcg_next() % term.width;
    uint32_t sy = lcg_next() % (term.height * 3 / 4);
    uint8_t br = (uint8_t)(lcg_next() % 180 + 50);
    uint32_t col2 = ((uint32_t)br << 16) | ((uint32_t)br << 8) | br;
    put_pixel(sx, sy, col2);
    if ((lcg_next() & 0xF) == 0) {
      put_pixel(sx + 1, sy, col2);
      put_pixel(sx, sy + 1, col2);
      put_pixel(sx + 1, sy + 1, col2);
    }
  }
}

// ---------- Progress bar (pixel-level, partial update only) ----------
static void splash_draw_bar(uint32_t filled_px) {
  uint32_t px = term.sp_bar_pixel_x;
  uint32_t py = term.sp_bar_pixel_y;
  uint32_t bw = term.sp_bar_w_px;
  uint32_t bh = term.sp_bar_h_px;

  uint32_t track = 0x1A3550;
  uint32_t border = 0x335577;

  // Refill only the interior (avoids redrawing border every step)
  for (uint32_t y = py + 1; y < py + bh - 1; y++) {
    // Empty region
    fill_rect(px + 1, y, bw - 2, 1, track);
    // Filled region (gradient)
    for (uint32_t x = px + 1; x < px + 1 + filled_px - 2 && x < px + bw - 1;
         x++)
      put_pixel(x, y, lerp_color(0x00D4FF, 0x33FFCC, x - (px + 1), bw));
  }

  // Border (drawn once here too, cheap)
  for (uint32_t x = px; x < px + bw; x++) {
    put_pixel(x, py, border);
    put_pixel(x, py + bh - 1, border);
  }
  for (uint32_t y = py; y < py + bh; y++) {
    put_pixel(px, y, border);
    put_pixel(px + bw - 1, y, border);
  }
  (void)track;
}

static void splash_clear_label(void) {
  uint32_t py = term.sp_label_row * FONT_H;
  fill_rect(0, py, term.width, FONT_H * 2, 0x0D1F35);
}

static const char *logo_lines[] = {
    "   ____                    _       __  ____   _____ ",
    "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ ",
    " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  ",
    "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  ",
    "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   ",
};
#define LOGO_ROWS 5

// ===========================================================================
// fb_splash_draw  — called right after fb_init(), before PMM exists
// ===========================================================================
void fb_splash_draw(const char *version, const char *arch) {
  if (!term.base)
    return;
  term.splash_active = 1;
  // backbuf is NULL here (PMM not yet up) — writes go directly to real FB

  // 1. Vertical gradient background
  for (uint32_t y = 0; y < term.height; y++) {
    uint32_t c = lerp_color(0x020D1A, 0x0D1F35, y, term.height);
    fast_fill32(term.base + y * term.pitch, c, term.width);
  }

  // 2. Star field
  splash_stars();

  // 3. Logo centred in upper 55% of screen
  uint32_t logo_total_h = LOGO_ROWS * FONT_H;
  uint32_t logo_area_h = term.height * 55 / 100;
  uint32_t logo_y0 =
      (logo_area_h > logo_total_h) ? (logo_area_h - logo_total_h) / 2 : 4;

  // Accent bar above logo (teal→cyan sweep, 3px, dimmed outer edges)
  {
    uint32_t y0 = (logo_y0 > 5) ? logo_y0 - 5 : 0;
    for (uint32_t dy = 0; dy < 3; dy++) {
      uint32_t y = y0 + dy;
      for (uint32_t x = 0; x < term.width; x++) {
        uint32_t col2 = lerp_color(0x008899, 0x33FFEE, x, term.width);
        if (dy == 0 || dy == 2) {
          uint32_t r = (col2 >> 16) & 0xFF, g = (col2 >> 8) & 0xFF,
                   b = col2 & 0xFF;
          col2 = ((r / 3) << 16) | ((g / 3) << 8) | (b / 3);
        }
        put_pixel(x, y, col2);
      }
    }
  }

  // Logo rows — each with its own colour
  static const uint32_t logo_cols[LOGO_ROWS] = {0x00EEFF, 0x00D4FF, 0x33DDCC,
                                                0x00B8FF, 0x7799FF};
  for (int li = 0; li < LOGO_ROWS; li++) {
    uint32_t py = logo_y0 + (uint32_t)li * FONT_H;
    uint32_t row = py / FONT_H;
    // Row background
    uint32_t bg_c = lerp_color(0x020D1A, 0x0D1F35, py, term.height);
    fast_fill32(term.base + py * term.pitch, bg_c, term.width * FONT_H);
    // Centred text
    const char *line = logo_lines[li];
    size_t len = 0;
    for (const char *p = line; *p; p++)
      len++;
    uint32_t col_start =
        (term.width > (uint32_t)(len * FONT_W))
            ? (term.width - (uint32_t)(len * FONT_W)) / 2 / FONT_W
            : 0;
    for (size_t ci = 0; line[ci]; ci++)
      draw_glyph_raw(col_start + (uint32_t)ci, row, line[ci], logo_cols[li], 0);
  }

  // 4. Tagline + feature strip below logo
  uint32_t tag_row = logo_y0 / FONT_H + LOGO_ROWS + 1;
  uint32_t feat_row = tag_row + 1;
  {
    static char tagbuf[64];
    const char *pre = "Quanta OS  v";
    size_t i = 0;
    for (const char *p = pre; *p && i < 60; p++)
      tagbuf[i++] = *p;
    for (const char *p = version; *p && i < 60; p++)
      tagbuf[i++] = *p;
    tagbuf[i++] = ' ';
    tagbuf[i++] = ' ';
    tagbuf[i++] = '(';
    for (const char *p = arch; *p && i < 60; p++)
      tagbuf[i++] = *p;
    tagbuf[i++] = ')';
    tagbuf[i] = '\0';
    fb_print_at((term.cols - i) / 2, tag_row, tagbuf, 0x778899, 0);
  }
  {
    const char *feats = "x2APIC  |  SMP  |  VirtIO  |  QuantaFS  |  KV  |  QAI";
    size_t len = 0;
    for (const char *p = feats; *p; p++)
      len++;
    uint32_t fc = (term.cols - len) / 2;
    static const uint32_t fcc[] = {0x33DDCC, 0x778899, 0x33DDCC,
                                   0x778899, 0x33DDCC, 0x778899,
                                   0x33DDCC, 0x778899, 0x33DDCC};
    int seg = 0;
    for (const char *p = feats; *p; p++, fc++) {
      uint32_t col2 = (*p == '|') ? 0x335577 : fcc[seg % 9];
      if (*p == '|')
        seg++;
      draw_glyph_raw(fc, feat_row, *p, col2, 0);
    }
  }

  // 5. Progress bar geometry (store for updates)
  term.sp_bar_pixel_y = term.height * 68 / 100;
  term.sp_bar_h_px = 16;
  term.sp_bar_w_px = term.width * 60 / 100;
  term.sp_bar_pixel_x = (term.width - term.sp_bar_w_px) / 2;
  term.sp_label_row = term.sp_bar_pixel_y / FONT_H + 2;

  // Draw empty bar
  splash_draw_bar(0);

  // "Starting..." above bar
  {
    const char *s = "Starting Quanta OS...";
    size_t sl = 0;
    for (const char *p = s; *p; p++)
      sl++;
    fb_print_at((term.cols - sl) / 2, term.sp_bar_pixel_y / FONT_H - 1, s,
                0x778899, 0);
  }

  // Bottom accent sweep
  {
    uint32_t by = term.height - FONT_H * 2;
    for (uint32_t x = 0; x < term.width; x++) {
      uint32_t col2 = lerp_color(0x33DDCC, 0x0044AA, x, term.width);
      put_pixel(x, by, col2);
      put_pixel(x, by + 1, lerp_color(col2, 0, 1, 2));
    }
  }
}

// ===========================================================================
// fb_splash_progress  — call after each init step
// ===========================================================================
void fb_splash_progress(int step, int total, const char *label) {
  if (!term.base || !term.splash_active)
    return;
  if (total <= 0)
    total = 1;

  uint32_t filled_px = (uint32_t)((uint64_t)step * term.sp_bar_w_px / total);
  splash_draw_bar(filled_px);

  // Percentage text centred inside bar
  {
    uint32_t pct = (uint32_t)(step * 100 / total);
    char pbuf[5];
    int pi = 0;
    if (pct >= 100) {
      pbuf[pi++] = '1';
      pbuf[pi++] = '0';
      pbuf[pi++] = '0';
    } else {
      if (pct >= 10)
        pbuf[pi++] = '0' + (char)(pct / 10);
      pbuf[pi++] = '0' + (char)(pct % 10);
    }
    pbuf[pi++] = '%';
    pbuf[pi] = '\0';
    uint32_t pr = term.sp_bar_pixel_y / FONT_H;
    uint32_t pc = term.cols / 2 - 1;
    // Clear old pct text first
    for (int k = 0; k < 4; k++)
      draw_glyph_raw(pc + k, pr, ' ', 0, 0);
    for (int k = 0; pbuf[k]; k++)
      draw_glyph_raw(pc + k, pr, pbuf[k], 0xFFFFFF, 0);
  }

  // Step label below bar
  splash_clear_label();
  if (label && *label) {
    size_t llen = 0;
    for (const char *p = label; *p; p++)
      llen++;
    uint32_t lc = (llen < term.cols) ? (term.cols - llen) / 2 : 0;
    fb_print_at(lc, term.sp_label_row, label, 0xAADDFF, 0);
  }

  // If back buffer is active, blit to screen now.
  // Without back buffer (early steps), drawing went directly to real FB.
  if (term.backbuf)
    fb_flip();
}

// ===========================================================================
// fb_splash_done  — TSC-timed fade + terminal resume
// ===========================================================================
void fb_splash_done(void) {
  if (!term.base)
    return;

  // If we had a back buffer, make sure latest frame is on screen
  if (term.backbuf)
    fb_flip();

  // Fade: 5 passes, each dims every pixel by 25%.
  // ~25 ms per step = ~125 ms total fade — visible on any CPU.
  uint32_t *fb = term.base;
  size_t n = (size_t)term.height * term.pitch;

  for (int step = 4; step >= 0; step--) {
    uint32_t dim = (uint32_t)step;
    for (size_t i = 0; i < n; i++) {
      uint32_t c = fb[i];
      uint32_t r = ((c >> 16) & 0xFF) * dim / 4;
      uint32_t g = ((c >> 8) & 0xFF) * dim / 4;
      uint32_t b = (c & 0xFF) * dim / 4;
      fb[i] = (r << 16) | (g << 8) | b;
    }
    // If we have a back buffer, update it too so it matches
    if (term.backbuf)
      fast_copy32(term.backbuf, fb, n);
    fb_spin_ms(25);
  }

  // Fast clear to black
  fast_fill32(fb, 0, n);

  // Re-enable terminal
  term.splash_active = 0;
  term.backbuf = 0; // release reference (caller frees PMM pages)
  term.cursor_x = 0;
  term.cursor_y = 0;

  fb_statusbar_refresh();
}
