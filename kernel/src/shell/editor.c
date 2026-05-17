// ============================================================
//  shell/editor.c — Quanta built-in text editor  v1.0
//
//  Layout (example on an 80×25 terminal):
//
//  ┌ Row 0  ─── title bar (filename + modified flag) ──────────────┐
//  │ Row 1                                                          │
//  │  …  content lines (line number gutter + text)                  │
//  │ Row rows-2                                                     │
//  └ Row rows-1 ─── keyboard-shortcut status bar ──────────────────┘
//  (system status bar lives in the row below at screen bottom)
//
//  The content area occupies rows 1 … (scroll_rows − 2).
//  scroll_rows is obtained from fb_get_size() which already
//  excludes the Quanta system status bar row.
// ============================================================

#include "editor.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../fs/vfs.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../version.h"
#include <stdbool.h>
#include <stddef.h>

// ── Configuration ─────────────────────────────────────────────────────────
#define ED_MAX_LINES 512 // max lines the editor can hold
#define ED_LINE_MAX 240  // max chars per line (display cols − gutter)
#define ED_TAB_WIDTH 4   // visual tab width

// ── Line storage ──────────────────────────────────────────────────────────
typedef struct {
  char buf[ED_LINE_MAX + 1];
  int len;
} ed_line_t;

// ── Global editor state (in BSS — no stack pressure) ─────────────────────
typedef struct {
  ed_line_t lines[ED_MAX_LINES];
  int count;  // number of lines (always >= 1)
  int cy;     // cursor row  (0-indexed into lines[])
  int cx;     // cursor col  (0-indexed byte position)
  int top;    // first visible line (scroll offset)
  bool dirty; // unsaved changes?
  char path[VFS_PATH_MAX];

  // One-line cut/paste buffer (^K / ^U)
  char clip[ED_LINE_MAX + 1];
  int clip_len;
  bool has_clip;
} ed_t;

static ed_t g_ed;

// ── Terminal geometry (filled on entry) ──────────────────────────────────
static int g_cols = 80;     // character columns
static int g_srows = 24;    // scroll_rows from fb_get_size
static int g_hdr_row = 0;   // title bar row
static int g_cnt_top = 1;   // first content row
static int g_cnt_bot = 22;  // last  content row (inclusive)
static int g_ftr_row = 23;  // keyboard-hint row
static int g_cnt_rows = 22; // number of content rows

#define GUTTER_W 5 // " NNN│" — 4 digits + separator

static void ed_update_geometry(void) {
  uint32_t c = 0, r = 0;
  fb_get_size(&c, &r);
  g_cols = (int)(c ? c : 80);
  g_srows = (int)(r ? r : 24);
  // Layout: row 0 = header, row srows-1 = footer, rest = content
  g_hdr_row = 0;
  g_ftr_row = g_srows - 1;
  g_cnt_top = 1;
  g_cnt_bot = g_srows - 2; // last content row (0-indexed)
  g_cnt_rows = g_cnt_bot - g_cnt_top + 1;
  if (g_cnt_rows < 2)
    g_cnt_rows = 2;
}

// ── ANSI cursor helpers ───────────────────────────────────────────────────
// All row/col parameters are 0-indexed; ANSI escape is 1-indexed.
static void cur_at(int row, int col) {
  kprintf("\033[%d;%dH", row + 1, col + 1);
}
static void cur_home(void) { kprintf("\033[H"); }
static void cur_clreol(void) { kprintf("\033[K"); }
static void cur_hide(void) { /* no xterm, just don't draw */ }

// ── Clear the editor content area ────────────────────────────────────────
static void ed_clear_content(void) {
  fb_set_color(0xFFFFFF, 0x000000);
  for (int r = g_cnt_top; r <= g_cnt_bot; r++) {
    cur_at(r, 0);
    cur_clreol();
  }
}

// ── Title bar ─────────────────────────────────────────────────────────────
static void ed_draw_header(void) {
  cur_at(g_hdr_row, 0);
  fb_set_color(0xFFFFFF, 0x1A3550);
  // Fill entire row with background
  for (int i = 0; i < g_cols; i++)
    kprintf(" ");

  // Left side: "Quanta Edit"
  cur_at(g_hdr_row, 1);
  fb_set_color(0x33DDCC, 0x1A3550);
  kprintf("Quanta Edit");
  fb_set_color(0xFFFFFF, 0x1A3550);
  kprintf("  ");

  // Filename (truncated to fit)
  {
    int fn_len = (int)strlen(g_ed.path);
    int max_fn = g_cols - 28; // reserve space for right side
    if (max_fn < 6)
      max_fn = 6;
    fb_set_color(0xAADDFF, 0x1A3550);
    if (fn_len > max_fn) {
      kprintf("...");
      kprintf("%s", g_ed.path + fn_len - (max_fn - 3));
    } else {
      kprintf("%s", g_ed.path);
    }
  }

  // Right side: modified / clean badge
  cur_at(g_hdr_row, g_cols - 12);
  if (g_ed.dirty) {
    fb_set_color(0xFF9922, 0x1A3550);
    kprintf("[Modified]  ");
  } else {
    fb_set_color(0x44EE88, 0x1A3550);
    kprintf("[  Clean ]  ");
  }
  fb_set_color(0xFFFFFF, 0x000000);
}

// ── Footer (keyboard shortcuts) ───────────────────────────────────────────
static void ed_draw_footer(void) {
  cur_at(g_ftr_row, 0);
  fb_set_color(0xFFFFFF, 0x1A3550);
  for (int i = 0; i < g_cols; i++)
    kprintf(" ");
  cur_at(g_ftr_row, 1);

  // Shortcut hints
  static const struct {
    const char *key;
    const char *desc;
  } hints[] = {
      {"^S", "Save"},  {"^Q", "Quit"}, {"^K", "Cut"},
      {"^U", "Paste"}, {"^G", "GoTo"}, {"^F", "Find"},
  };
  for (int i = 0; i < 6; i++) {
    fb_set_color(0xFFFF55, 0x1A3550);
    kprintf(" %s", hints[i].key);
    fb_set_color(0xCCCCCC, 0x1A3550);
    kprintf(":%s", hints[i].desc);
  }

  // Position indicator (right-aligned)
  cur_at(g_ftr_row, g_cols - 18);
  fb_set_color(0xAADDFF, 0x1A3550);
  kprintf("Ln:%-4d Col:%-3d", g_ed.cy + 1, g_ed.cx + 1);
  fb_set_color(0xFFFFFF, 0x000000);
}

// ── Render a single content row (screen_row = g_cnt_top .. g_cnt_bot) ────
static void ed_draw_line(int screen_row, int ed_row) {
  cur_at(screen_row, 0);

  if (ed_row >= g_ed.count) {
    // Past end of file — show tilde marker
    fb_set_color(0x335577, 0x000000);
    kprintf("   ~");
    fb_set_color(0xFFFFFF, 0x000000);
    cur_clreol();
    return;
  }

  const ed_line_t *ln = &g_ed.lines[ed_row];
  bool is_cur = (ed_row == g_ed.cy);

  // Line number gutter
  fb_set_color(0x778899, is_cur ? 0x1A2030 : 0x000000);
  kprintf("%4d", ed_row + 1);
  fb_set_color(0x335577, is_cur ? 0x1A2030 : 0x000000);
  kprintf("\xB3"); // CP437 │

  // Content
  uint32_t txt_fg = is_cur ? 0xFFFFFF : 0xDDDDDD;
  uint32_t txt_bg = is_cur ? 0x1A2030 : 0x000000;
  fb_set_color(txt_fg, txt_bg);

  int max_display = g_cols - GUTTER_W;
  int col = 0;
  for (int i = 0; i < ln->len && col < max_display; i++) {
    char c = ln->buf[i];
    if (c == '\t') {
      int spaces = ED_TAB_WIDTH - (col % ED_TAB_WIDTH);
      while (spaces-- > 0 && col < max_display) {
        kprintf(" ");
        col++;
      }
    } else if ((unsigned char)c >= 0x20) {
      kprintf("%c", c);
      col++;
    }
  }
  cur_clreol();
  fb_set_color(0xFFFFFF, 0x000000);
}

// ── Full redraw ───────────────────────────────────────────────────────────
static void ed_render(void) {
  ed_draw_header();

  for (int i = 0; i < g_cnt_rows; i++)
    ed_draw_line(g_cnt_top + i, g_ed.top + i);

  ed_draw_footer();

  // Place the hardware cursor at the edit point
  int scr_row = g_cnt_top + (g_ed.cy - g_ed.top);
  int scr_col = GUTTER_W + g_ed.cx;
  if (scr_col >= g_cols)
    scr_col = g_cols - 1;
  cur_at(scr_row, scr_col);
}

// ── Scroll so cursor stays in the visible content window ─────────────────
static void ed_scroll(void) {
  if (g_ed.cy < g_ed.top)
    g_ed.top = g_ed.cy;
  if (g_ed.cy >= g_ed.top + g_cnt_rows)
    g_ed.top = g_ed.cy - g_cnt_rows + 1;
  if (g_ed.top < 0)
    g_ed.top = 0;
}

// ── Clamp cursor column to current line length ────────────────────────────
static void ed_clamp_cx(void) {
  int maxcx = (g_ed.cy < g_ed.count) ? g_ed.lines[g_ed.cy].len : 0;
  if (g_ed.cx > maxcx)
    g_ed.cx = maxcx;
}

// ── Insert one printable character at cursor ──────────────────────────────
static void ed_insert_char(char c) {
  if (g_ed.cy >= ED_MAX_LINES)
    return;
  // Grow count if needed (cursor can be on a virtual blank line)
  while (g_ed.count <= g_ed.cy) {
    g_ed.lines[g_ed.count].len = 0;
    g_ed.lines[g_ed.count].buf[0] = '\0';
    g_ed.count++;
  }
  ed_line_t *ln = &g_ed.lines[g_ed.cy];
  if (ln->len >= ED_LINE_MAX)
    return;
  // Shift right
  for (int i = ln->len; i > g_ed.cx; i--)
    ln->buf[i] = ln->buf[i - 1];
  ln->buf[g_ed.cx] = c;
  ln->len++;
  ln->buf[ln->len] = '\0';
  g_ed.cx++;
  g_ed.dirty = true;
}

// ── Enter key: split current line at cursor ───────────────────────────────
static void ed_enter(void) {
  if (g_ed.count >= ED_MAX_LINES - 1)
    return;
  // Ensure current line exists
  while (g_ed.count <= g_ed.cy) {
    g_ed.lines[g_ed.count].len = 0;
    g_ed.lines[g_ed.count].buf[0] = '\0';
    g_ed.count++;
  }
  ed_line_t *cur = &g_ed.lines[g_ed.cy];
  int right = cur->len - g_ed.cx;

  // Shift all lines below down by one
  for (int i = g_ed.count; i > g_ed.cy + 1; i--)
    g_ed.lines[i] = g_ed.lines[i - 1];
  g_ed.count++;

  // New line gets the tail of the current line
  ed_line_t *nxt = &g_ed.lines[g_ed.cy + 1];
  if (right > 0) {
    memcpy(nxt->buf, cur->buf + g_ed.cx, (size_t)right);
  }
  nxt->len = right;
  nxt->buf[right] = '\0';

  // Truncate current line at cursor
  cur->len = g_ed.cx;
  cur->buf[g_ed.cx] = '\0';

  g_ed.cy++;
  g_ed.cx = 0;
  g_ed.dirty = true;
}

// ── Backspace ─────────────────────────────────────────────────────────────
static void ed_backspace(void) {
  if (g_ed.cy == 0 && g_ed.cx == 0)
    return;
  if (g_ed.cy >= g_ed.count) {
    g_ed.cy = g_ed.count - 1;
    g_ed.cx = g_ed.lines[g_ed.cy].len;
    return;
  }
  ed_line_t *cur = &g_ed.lines[g_ed.cy];

  if (g_ed.cx > 0) {
    // Delete char to the left
    for (int i = g_ed.cx - 1; i < cur->len - 1; i++)
      cur->buf[i] = cur->buf[i + 1];
    cur->len--;
    cur->buf[cur->len] = '\0';
    g_ed.cx--;
  } else {
    // Join with previous line
    ed_line_t *prv = &g_ed.lines[g_ed.cy - 1];
    int old_prv = prv->len;
    if (prv->len + cur->len <= ED_LINE_MAX) {
      memcpy(prv->buf + prv->len, cur->buf, (size_t)cur->len);
      prv->len += cur->len;
      prv->buf[prv->len] = '\0';
      // Shift lines up
      for (int i = g_ed.cy; i < g_ed.count - 1; i++)
        g_ed.lines[i] = g_ed.lines[i + 1];
      g_ed.count--;
      g_ed.cy--;
      g_ed.cx = old_prv;
    }
  }
  g_ed.dirty = true;
}

// ── Delete (forward) ──────────────────────────────────────────────────────
static void ed_delete(void) {
  if (g_ed.cy >= g_ed.count)
    return;
  ed_line_t *cur = &g_ed.lines[g_ed.cy];
  if (g_ed.cx < cur->len) {
    for (int i = g_ed.cx; i < cur->len - 1; i++)
      cur->buf[i] = cur->buf[i + 1];
    cur->len--;
    cur->buf[cur->len] = '\0';
  } else if (g_ed.cy < g_ed.count - 1) {
    // Join with next line
    ed_line_t *nxt = &g_ed.lines[g_ed.cy + 1];
    if (cur->len + nxt->len <= ED_LINE_MAX) {
      memcpy(cur->buf + cur->len, nxt->buf, (size_t)nxt->len);
      cur->len += nxt->len;
      cur->buf[cur->len] = '\0';
      for (int i = g_ed.cy + 1; i < g_ed.count - 1; i++)
        g_ed.lines[i] = g_ed.lines[i + 1];
      g_ed.count--;
    }
  }
  g_ed.dirty = true;
}

// ── ^K: cut current line ──────────────────────────────────────────────────
static void ed_cut_line(void) {
  if (g_ed.cy >= g_ed.count)
    return;
  ed_line_t *cur = &g_ed.lines[g_ed.cy];
  memcpy(g_ed.clip, cur->buf, (size_t)cur->len);
  g_ed.clip_len = cur->len;
  g_ed.clip[g_ed.clip_len] = '\0';
  g_ed.has_clip = true;

  if (g_ed.count > 1) {
    for (int i = g_ed.cy; i < g_ed.count - 1; i++)
      g_ed.lines[i] = g_ed.lines[i + 1];
    g_ed.count--;
    if (g_ed.cy >= g_ed.count)
      g_ed.cy = g_ed.count - 1;
  } else {
    cur->len = 0;
    cur->buf[0] = '\0';
  }
  g_ed.cx = 0;
  g_ed.dirty = true;
}

// ── ^U: paste cut line (insert before current line) ───────────────────────
static void ed_uncut(void) {
  if (!g_ed.has_clip || g_ed.count >= ED_MAX_LINES - 1)
    return;
  // Shift lines down
  for (int i = g_ed.count; i > g_ed.cy; i--)
    g_ed.lines[i] = g_ed.lines[i - 1];
  g_ed.count++;
  ed_line_t *nl = &g_ed.lines[g_ed.cy];
  memcpy(nl->buf, g_ed.clip, (size_t)g_ed.clip_len);
  nl->len = g_ed.clip_len;
  nl->buf[nl->len] = '\0';
  g_ed.cx = 0;
  g_ed.dirty = true;
}

// ── Save ──────────────────────────────────────────────────────────────────
static int ed_save(void) {
  int fd = vfs_open(g_ed.path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0)
    return -1;
  for (int i = 0; i < g_ed.count; i++) {
    vfs_write(fd, g_ed.lines[i].buf, (size_t)g_ed.lines[i].len);
    vfs_write(fd, "\n", 1);
  }
  vfs_close(fd);
  g_ed.dirty = false;
  return 0;
}

// ── Load ──────────────────────────────────────────────────────────────────
static void ed_load(void) {
  // Reset state
  g_ed.count = 1;
  g_ed.lines[0].len = 0;
  g_ed.lines[0].buf[0] = '\0';

  int fd = vfs_open(g_ed.path, O_RDONLY);
  if (fd < 0)
    return; // new file — leave with one empty line

  int cur_line = 0;
  int cur_col = 0;
  char buf[512];
  ssize_t n;

  while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\r')
        continue;
      if (c == '\n') {
        g_ed.lines[cur_line].len = cur_col;
        g_ed.lines[cur_line].buf[cur_col] = '\0';
        cur_line++;
        if (cur_line >= ED_MAX_LINES - 1)
          goto load_done;
        cur_col = 0;
        g_ed.lines[cur_line].len = 0;
        g_ed.lines[cur_line].buf[0] = '\0';
      } else if (cur_col < ED_LINE_MAX) {
        g_ed.lines[cur_line].buf[cur_col++] = c;
      }
    }
  }

load_done:
  // Finalise last line
  g_ed.lines[cur_line].len = cur_col;
  g_ed.lines[cur_line].buf[cur_col] = '\0';
  g_ed.count = cur_line + 1;
  if (g_ed.count < 1)
    g_ed.count = 1;

  vfs_close(fd);
}

// ── Inline message in the footer row ─────────────────────────────────────
static void ed_msg(const char *text) {
  cur_at(g_ftr_row, 0);
  fb_set_color(0xFFFFFF, 0x332200);
  for (int i = 0; i < g_cols; i++)
    kprintf(" ");
  cur_at(g_ftr_row, 1);
  kprintf(" %s", text);
  fb_set_color(0xFFFFFF, 0x000000);
}

// ── Yes/No confirm ────────────────────────────────────────────────────────
static bool ed_confirm(const char *prompt) {
  ed_msg(prompt);
  // Append " [Y/N]" hint
  kprintf("  [Y/N]");
  for (;;) {
    char c = kbd_getchar();
    if (c == 'y' || c == 'Y')
      return true;
    if (c == 'n' || c == 'N' || c == '\033' || c == 3)
      return false;
  }
}

// ── Mini input line at footer ─────────────────────────────────────────────
static bool ed_readline(const char *prompt, char *out, int maxlen) {
  cur_at(g_ftr_row, 0);
  fb_set_color(0xFFFFFF, 0x1A3550);
  for (int i = 0; i < g_cols; i++)
    kprintf(" ");
  cur_at(g_ftr_row, 1);
  fb_set_color(0xFFFF55, 0x1A3550);
  kprintf(" %s", prompt);
  fb_set_color(0xFFFFFF, 0x1A3550);

  int pos = 0;
  out[0] = '\0';
  for (;;) {
    char c = kbd_getchar();
    if (c == '\n' || c == '\r') {
      out[pos] = '\0';
      fb_set_color(0xFFFFFF, 0x000000);
      return true;
    }
    if (c == '\033' || c == 3) {
      fb_set_color(0xFFFFFF, 0x000000);
      return false;
    }
    if ((c == '\b' || c == 127) && pos > 0) {
      pos--;
      kprintf("\b \b");
    } else if (c >= 0x20 && c < 0x7F && pos < maxlen - 1) {
      out[pos++] = c;
      kprintf("%c", c);
    }
  }
}

// ── ^F: find (simple substring search forward from cursor) ────────────────
static void ed_find(void) {
  char pat[64];
  if (!ed_readline("Find: ", pat, sizeof(pat)))
    return;
  if (!pat[0])
    return;
  size_t plen = strlen(pat);

  // Search from cy+1 (or cx+1 on same line) forward, wrapping
  for (int pass = 0; pass < 2; pass++) {
    for (int r = (pass == 0 ? g_ed.cy : 0); r < g_ed.count; r++) {
      const char *haystack = g_ed.lines[r].buf;
      int start = (pass == 0 && r == g_ed.cy) ? g_ed.cx + 1 : 0;
      int hlen = g_ed.lines[r].len;
      for (int c = start; c + (int)plen <= hlen; c++) {
        if (memcmp(haystack + c, pat, plen) == 0) {
          g_ed.cy = r;
          g_ed.cx = c;
          ed_scroll();
          ed_render();
          ed_msg("Found.");
          return;
        }
      }
    }
  }
  ed_msg("Not found.");
  // Pause briefly so the user can see the message, then re-render
  for (volatile int i = 0; i < 30000000; i++)
    __asm__ volatile("pause");
}

// ── Help overlay ──────────────────────────────────────────────────────────
static void ed_help(void) {
  // Draw over content area
  cur_at(g_cnt_top, 0);
  fb_set_color(0xFFFFFF, 0x1A3550);
  for (int r = g_cnt_top; r <= g_cnt_bot; r++) {
    cur_at(r, 0);
    for (int c = 0; c < g_cols; c++)
      kprintf(" ");
  }
  int r = g_cnt_top + 1, m = g_cols / 2 - 20;
  cur_at(r++, m);
  fb_set_color(0x33DDCC, 0x1A3550);
  kprintf("  Quanta Editor — Keyboard Reference");
  cur_at(r++, m);
  fb_set_color(0x335577, 0x1A3550);
  kprintf("  ─────────────────────────────────────");

  static const char *keys[][2] = {
      {"Arrows / Home / End", "Move cursor"},
      {"PgUp / PgDn", "Scroll page"},
      {"^S", "Save file"},
      {"^Q", "Quit (prompts if unsaved)"},
      {"^K", "Cut current line"},
      {"^U", "Paste cut line before cursor"},
      {"^G", "Go to line number"},
      {"^F", "Find substring (wraps)"},
      {"^H", "This help screen"},
      {"Enter", "Split line"},
      {"Backspace / Delete", "Delete character"},
  };
  for (int i = 0; i < 11; i++) {
    cur_at(r++, m);
    fb_set_color(0xFFFF55, 0x1A3550);
    kprintf("  %-22s", keys[i][0]);
    fb_set_color(0xCCCCCC, 0x1A3550);
    kprintf("%s", keys[i][1]);
  }
  r++;
  cur_at(r, m);
  fb_set_color(0x778899, 0x1A3550);
  kprintf("  Press any key to return to editing…");
  fb_set_color(0xFFFFFF, 0x000000);
  kbd_getchar(); // wait for any key
}

// ── Entry point ───────────────────────────────────────────────────────────
int ed_open(const char *path) {
  if (!path || !*path)
    return -1;

  // Reset state
  memset(&g_ed, 0, sizeof(g_ed));
  strncpy(g_ed.path, path, VFS_PATH_MAX - 1);
  g_ed.path[VFS_PATH_MAX - 1] = '\0';
  g_ed.count = 1;
  g_ed.cy = g_ed.cx = g_ed.top = 0;
  g_ed.dirty = false;
  g_ed.has_clip = false;

  ed_update_geometry();
  ed_load();

  // Clear the scrollable content area
  kprintf("\033[2J");
  cur_home();
  fb_set_color(0xFFFFFF, 0x000000);

  // Override system status bar with editor hint
  fb_statusbar_set(
      "Quanta Editor  |  ^S:Save  ^Q:Quit  ^K:Cut  ^U:Paste  ^F:Find  ^H:Help");
  fb_statusbar_refresh();

  ed_render();

  bool running = true;
  while (running) {
    char c = kbd_getchar();

    // ── Escape sequences (arrow / function keys) ──────────────────────
    if (c == '\033') {
      char s1 = kbd_getchar_noblock();
      if (!s1) {
        // Bare ESC — ignore
        continue;
      }
      if (s1 != '[') {
        continue;
      }
      char s2 = kbd_getchar_noblock();
      if (!s2)
        s2 = kbd_getchar(); // wait for it if not buffered yet

      switch (s2) {
      case 'A': // Up
        if (g_ed.cy > 0) {
          g_ed.cy--;
          ed_clamp_cx();
        }
        break;
      case 'B': // Down
        if (g_ed.cy < g_ed.count - 1) {
          g_ed.cy++;
          ed_clamp_cx();
        }
        break;
      case 'C': // Right
        if (g_ed.cy < g_ed.count) {
          if (g_ed.cx < g_ed.lines[g_ed.cy].len)
            g_ed.cx++;
          else if (g_ed.cy < g_ed.count - 1) {
            g_ed.cy++;
            g_ed.cx = 0;
          }
        }
        break;
      case 'D': // Left
        if (g_ed.cx > 0)
          g_ed.cx--;
        else if (g_ed.cy > 0) {
          g_ed.cy--;
          g_ed.cx = g_ed.lines[g_ed.cy].len;
        }
        break;
      case 'H': // Home
        g_ed.cx = 0;
        break;
      case 'F': // End
        if (g_ed.cy < g_ed.count)
          g_ed.cx = g_ed.lines[g_ed.cy].len;
        break;
      case '5': { // Page Up — format: ESC [ 5 ~
        char tilde = kbd_getchar_noblock();
        if (!tilde)
          tilde = kbd_getchar();
        (void)tilde;
        g_ed.cy -= g_cnt_rows - 1;
        if (g_ed.cy < 0)
          g_ed.cy = 0;
        ed_clamp_cx();
        break;
      }
      case '6': { // Page Down — ESC [ 6 ~
        char tilde = kbd_getchar_noblock();
        if (!tilde)
          tilde = kbd_getchar();
        (void)tilde;
        g_ed.cy += g_cnt_rows - 1;
        if (g_ed.cy >= g_ed.count)
          g_ed.cy = g_ed.count - 1;
        ed_clamp_cx();
        break;
      }
      case '3': { // Delete key — ESC [ 3 ~
        char tilde = kbd_getchar_noblock();
        if (!tilde)
          tilde = kbd_getchar();
        (void)tilde;
        ed_delete();
        break;
      }
      default:
        break;
      }

      // ── Control characters ─────────────────────────────────────────────
    } else if (c == 19) { // ^S — save
      if (ed_save() == 0)
        ed_msg("  Saved.");
      else
        ed_msg("  ERROR: save failed — check file path.");

    } else if (c == 17) { // ^Q — quit
      if (!g_ed.dirty || ed_confirm("Unsaved changes. Quit without saving?")) {
        running = false;
        continue;
      }

    } else if (c == 11) { // ^K — cut line
      ed_cut_line();

    } else if (c == 21) { // ^U — paste/uncut
      ed_uncut();

    } else if (c == 7) { // ^G — go to line
      char num[16];
      if (ed_readline("Go to line: ", num, sizeof(num))) {
        int n = 0;
        for (const char *p = num; *p >= '0' && *p <= '9'; p++)
          n = n * 10 + (*p - '0');
        if (n >= 1 && n <= g_ed.count) {
          g_ed.cy = n - 1;
          g_ed.cx = 0;
        }
      }

    } else if (c == 6) { // ^F — find
      ed_find();
      continue; // ed_find handles render itself

    } else if (c == 8) { // ^H — help
      ed_help();

    } else if (c == '\n' || c == '\r') {
      ed_enter();

    } else if (c == '\b' || c == 127) {
      ed_backspace();

    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
      ed_insert_char(c);
    }

    // After every edit action: scroll and full redraw
    ed_scroll();
    ed_render();
  }

  // ── Clean exit ────────────────────────────────────────────────────────
  kprintf("\033[2J\033[H");
  fb_set_color(0xFFFFFF, 0x000000);
  fb_statusbar_set("Quanta OS v" QUANTA_VERSION
                   "  |  help  top  free  ls  edit  calc  ai");
  fb_statusbar_refresh();
  return 0;
}
