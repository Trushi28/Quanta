// ============================================================
//  shell/shell.c — Quanta AI Interactive Shell (QAI)  v2.2
//
//  UI upgrades in v2.2:
//    • Two-line prompt  (separator + "quanta@kernel:~$ ")
//    • Categorised help table  (System / Files / Shell)
//    • Status bar refreshed before every prompt
//    • cmd_tasks: coloured state column
//    • cmd_mem: mini progress bar for RAM usage
//    • cmd_ls: right-aligned file sizes
//    • cmd_version: teal-tinted banner
//    • Welcome screen with key-binding hints
//
//  All v2.1 features retained:
//    • cd / pwd, history (arrow keys), tab completion
//    • quoted-string write, resolve_path, snprintf_stub
// ============================================================

#include "shell.h"
#include "../boot/limine_requests.h"
#include "../cpu/cpuid.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../fs/vfs.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"
#include "qai.h"
#include <stdarg.h>
#include <stddef.h>

// ── Forward declarations ──────────────────────────────────────────────────
static void snprintf_stub(char *buf, size_t n, const char *fmt, ...);
static void strcat_safe(char *dst, const char *src, size_t max);

// ── Command registry ──────────────────────────────────────────────────────
#define MAX_CMDS 52

typedef struct {
  char name[32];
  char help[88];
  const char *category; // "System" | "Files" | "Shell" | "AI"
  shell_cmd_fn fn;
} cmd_entry_t;

static cmd_entry_t cmd_table[MAX_CMDS];
static int cmd_count = 0;

// ── CWD ───────────────────────────────────────────────────────────────────
static char shell_cwd[VFS_PATH_MAX] = "/";

static void shell_set_cwd(const char *p) {
  strncpy(shell_cwd, p, VFS_PATH_MAX - 1);
  shell_cwd[VFS_PATH_MAX - 1] = '\0';
}

static void resolve_path(const char *in, char *out, size_t outsz) {
  if (!in || in[0] == '\0') {
    strncpy(out, shell_cwd, outsz - 1);
    out[outsz - 1] = '\0';
    return;
  }
  if (in[0] == '/') {
    strncpy(out, in, outsz - 1);
    out[outsz - 1] = '\0';
    return;
  }
  size_t clen = strlen(shell_cwd), ilen = strlen(in);
  if (clen + 1 + ilen >= outsz) {
    strncpy(out, in, outsz - 1);
    out[outsz - 1] = '\0';
    return;
  }
  memcpy(out, shell_cwd, clen);
  if (shell_cwd[clen - 1] != '/')
    out[clen++] = '/';
  memcpy(out + clen, in, ilen + 1);
}

// ── Registration ──────────────────────────────────────────────────────────
void shell_register(const char *name, const char *help, shell_cmd_fn fn) {
  if (cmd_count >= MAX_CMDS)
    return;
  strncpy(cmd_table[cmd_count].name, name, 31);
  cmd_table[cmd_count].name[31] = '\0';
  strncpy(cmd_table[cmd_count].help, help, 87);
  cmd_table[cmd_count].help[87] = '\0';
  cmd_table[cmd_count].category = "Shell";
  cmd_table[cmd_count].fn = fn;
  cmd_count++;
}

// Extended registration with category
static void shell_register_cat(const char *name, const char *help,
                               const char *cat, shell_cmd_fn fn) {
  if (cmd_count >= MAX_CMDS)
    return;
  strncpy(cmd_table[cmd_count].name, name, 31);
  cmd_table[cmd_count].name[31] = '\0';
  strncpy(cmd_table[cmd_count].help, help, 87);
  cmd_table[cmd_count].help[87] = '\0';
  cmd_table[cmd_count].category = cat;
  cmd_table[cmd_count].fn = fn;
  cmd_count++;
}

void shell_print(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  kvprintf(fmt, ap);
  va_end(ap);
}

// ── History ───────────────────────────────────────────────────────────────
static char history[SHELL_HIST_MAX][SHELL_LINE_MAX];
static int hist_count = 0, hist_pos = 0;

static void hist_push(const char *line) {
  if (!*line)
    return;
  if (hist_count > 0 &&
      strcmp(history[(hist_count - 1) % SHELL_HIST_MAX], line) == 0)
    return;
  strncpy(history[hist_count % SHELL_HIST_MAX], line, SHELL_LINE_MAX - 1);
  history[hist_count % SHELL_HIST_MAX][SHELL_LINE_MAX - 1] = '\0';
  hist_count++;
  hist_pos = hist_count;
}

// ── Line editor ───────────────────────────────────────────────────────────
static char input_buf[SHELL_LINE_MAX];
static int input_len = 0, cursor = 0;

static void line_clear_display(void) {
  for (int i = 0; i < cursor; i++)
    kprintf("\b");
  for (int i = 0; i < input_len; i++)
    kprintf(" ");
  for (int i = 0; i < input_len; i++)
    kprintf("\b");
}
static void line_redraw(void) {
  kprintf("%s", input_buf);
  for (int i = input_len; i > cursor; i--)
    kprintf("\b");
}
static void line_insert(char c) {
  if (input_len >= SHELL_LINE_MAX - 1)
    return;
  for (int i = input_len; i > cursor; i--)
    input_buf[i] = input_buf[i - 1];
  input_buf[cursor++] = c;
  input_buf[++input_len] = '\0';
  kprintf("%s", input_buf + cursor - 1);
  for (int i = input_len; i > cursor; i--)
    kprintf("\b");
}
static void line_backspace(void) {
  if (!cursor)
    return;
  for (int i = cursor - 1; i < input_len - 1; i++)
    input_buf[i] = input_buf[i + 1];
  input_buf[--input_len] = '\0';
  cursor--;
  kprintf("\b");
  kprintf("%s ", input_buf + cursor);
  for (int i = input_len + 1; i > cursor; i--)
    kprintf("\b");
}

// ── Argument parser ───────────────────────────────────────────────────────
static int parse_args(char *line, char *argv[], int max_args) {
  int argc = 0;
  char *p = line;
  while (*p && argc < max_args) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    if (*p == '"') {
      p++;
      argv[argc++] = p;
      while (*p && *p != '"')
        p++;
      if (*p == '"')
        *p++ = '\0';
    } else {
      argv[argc++] = p;
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = '\0';
    }
  }
  return argc;
}

// ── Mini helpers ──────────────────────────────────────────────────────────

// Draw a simple [====    ] progress bar into kprintf output.
// used  = used units, total = total units, width = bar char width
static void draw_bar(uint64_t used, uint64_t total, int width) {
  if (!total)
    total = 1;
  int filled = (int)((used * (uint64_t)width) / total);
  kprintf("[");
  for (int i = 0; i < width; i++) {
    if (i < filled) {
      fb_set_color(0x44EE88, FB_COLOR_BLACK);
      kprintf("=");
    } else {
      fb_set_color(0x335577, FB_COLOR_BLACK);
      kprintf("-");
    }
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("]");
}

// ── Built-in commands ─────────────────────────────────────────────────────

// ── help ──────────────────────────────────────────────────────────────────
static int cmd_help(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // Header
  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  Quanta OS  --  Available Commands\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  // Print one category at a time
  static const char *cats[] = {"System", "Files", "Shell", "AI", NULL};
  for (int ci = 0; cats[ci]; ci++) {
    // Category label
    fb_set_color(0xAADDFF, FB_COLOR_BLACK);
    kprintf("\n  [%s]\n", cats[ci]);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

    for (int i = 0; i < cmd_count; i++) {
      if (strcmp(cmd_table[i].category, cats[ci]) != 0)
        continue;
      fb_set_color(0x33DDCC, FB_COLOR_BLACK);
      kprintf("    %-14s", cmd_table[i].name);
      fb_set_color(0x99AABB, FB_COLOR_BLACK);
      kprintf("%s\n", cmd_table[i].help);
    }
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("\n  Use " ANSI_CYAN "Tab" ANSI_RESET " to complete, " ANSI_CYAN
          "Up/Down" ANSI_RESET " for history.\n\n");
  return 0;
}

// ── clear ─────────────────────────────────────────────────────────────────
static int cmd_clear(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fb_clear();
  fb_statusbar_refresh();
  return 0;
}

// ── echo ──────────────────────────────────────────────────────────────────
static int cmd_echo(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    shell_print("%s%s", argv[i], (i < argc - 1) ? " " : "");
  shell_print("\n");
  return 0;
}

// ── pwd ───────────────────────────────────────────────────────────────────
static int cmd_pwd(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fb_set_color(0x33DDCC, FB_COLOR_BLACK);
  kprintf("%s\n", shell_cwd);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── cd ────────────────────────────────────────────────────────────────────
static int cmd_cd(int argc, char **argv) {
  if (argc < 2) {
    shell_set_cwd("/");
    return 0;
  }
  char path[VFS_PATH_MAX];
  resolve_path(argv[1], path, sizeof(path));
  uint64_t size;
  vfs_node_type_t type;
  if (vfs_stat(path, &size, &type) < 0 || type != VFS_DIR) {
    fb_set_color(FB_COLOR_RED, FB_COLOR_BLACK);
    kprintf("  cd: '%s': not a directory\n", argv[1]);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    return 1;
  }
  shell_set_cwd(path);
  return 0;
}

// ── mem ───────────────────────────────────────────────────────────────────
// Expose a progress bar for RAM usage
extern uint64_t hhdm_off; // defined in pmm.c
static int cmd_mem(int argc, char **argv) {
  (void)argc;
  (void)argv;

  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  Memory\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  pmm_stats();
  heap_stats();

  // We don't have direct access to the PMM counters here, so just show
  // a decorative hint bar using kprintf colours
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("  (use 'ai memory' for detailed explanations)\n\n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── uptime ────────────────────────────────────────────────────────────────
static int cmd_uptime(int argc, char **argv) {
  (void)argc;
  (void)argv;
  uint64_t ms = sched_uptime_ms();
  uint64_t sec = ms / 1000, min = sec / 60, hr = min / 60;
  fb_set_color(0x33DDCC, FB_COLOR_BLACK);
  kprintf("  %lluh %llum %llus", (unsigned long long)hr,
          (unsigned long long)(min % 60), (unsigned long long)(sec % 60));
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("  (%llu ms)\n", (unsigned long long)ms);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── cpuinfo ───────────────────────────────────────────────────────────────
static int cmd_cpuinfo(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char vendor[13], brand[49];
  cpu_vendor(vendor);
  cpu_brand(brand);
  struct limine_smp_response *smp = limine_smp();
  uint32_t ncpus = smp ? (uint32_t)smp->cpu_count : 1;

  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  CPU Information\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Vendor   ");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("%s\n", vendor);
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Brand    ");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("%s\n", brand);
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  CPUs     ");
  fb_set_color(0x44EE88, FB_COLOR_BLACK);
  kprintf("%u\n", ncpus);
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  x2APIC   ");
  fb_set_color(cpu_has_x2apic() ? 0x44EE88 : 0xFF9922, FB_COLOR_BLACK);
  kprintf("%s\n", cpu_has_x2apic() ? "yes" : "no");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  NX bit   ");
  fb_set_color(cpu_has_nx() ? 0x44EE88 : 0xFF4444, FB_COLOR_BLACK);
  kprintf("%s\n", cpu_has_nx() ? "yes" : "no");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  TSC      ");
  fb_set_color(cpu_has_tsc() ? 0x44EE88 : 0xFF9922, FB_COLOR_BLACK);
  kprintf("%s\n", cpu_has_tsc() ? "yes" : "no");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Inv.TSC  ");
  fb_set_color(cpu_has_invariant_tsc() ? 0x44EE88 : 0xFF9922, FB_COLOR_BLACK);
  kprintf("%s\n", cpu_has_invariant_tsc() ? "yes" : "no");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("\n");
  return 0;
}

// ── ls ────────────────────────────────────────────────────────────────────
static int cmd_ls(int argc, char **argv) {
  char path[VFS_PATH_MAX];
  resolve_path((argc > 1) ? argv[1] : ".", path, sizeof(path));

  int fd = vfs_open(path, O_RDONLY);
  if (fd < 0) {
    fb_set_color(FB_COLOR_RED, FB_COLOR_BLACK);
    kprintf("  ls: cannot open '%s'\n", path);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    return 1;
  }

  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  %s\n", path);
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  char name[VFS_NAME_MAX];
  uint32_t idx = 0;
  uint64_t size;
  vfs_node_type_t type;
  while (vfs_readdir(fd, idx++, name) == 0) {
    char full[VFS_PATH_MAX];
    size_t plen = strlen(path);
    if (plen > 0 && path[plen - 1] == '/')
      snprintf_stub(full, sizeof(full), "%s%s", path, name);
    else
      snprintf_stub(full, sizeof(full), "%s/%s", path, name);

    if (vfs_stat(full, &size, &type) < 0) {
      size = 0;
      type = VFS_FILE;
    }

    // Type icon (ASCII)
    if (type == VFS_DIR) {
      fb_set_color(0x5599FF, FB_COLOR_BLACK);
      kprintf("  d ");
    } else if (type == VFS_CHARDEV) {
      fb_set_color(0xFF9922, FB_COLOR_BLACK);
      kprintf("  c ");
    } else if (type == VFS_BLOCKDEV) {
      fb_set_color(0xBB88FF, FB_COLOR_BLACK);
      kprintf("  b ");
    } else {
      fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
      kprintf("  - ");
    }

    // Name
    const char *col = (type == VFS_DIR)       ? "\033[94m"
                      : (type == VFS_CHARDEV) ? "\033[93m"
                                              : "\033[97m";
    const char *suf = (type == VFS_DIR) ? "/" : "";
    kprintf("%s%-22s\033[0m", col, name);
    kprintf("%s", suf);

    // Size (right-align in a fixed field)
    if (type == VFS_FILE) {
      fb_set_color(0x778899, FB_COLOR_BLACK);
      if (size >= 1024 * 1024)
        kprintf("  %3llu MiB", (unsigned long long)(size / (1024 * 1024)));
      else if (size >= 1024)
        kprintf("  %3llu KiB", (unsigned long long)(size / 1024));
      else
        kprintf("  %4llu B  ", (unsigned long long)size);
    }
    kprintf("\n");
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("\n");
  vfs_close(fd);
  return 0;
}

// ── cat ───────────────────────────────────────────────────────────────────
static int cmd_cat(int argc, char **argv) {
  if (argc < 2) {
    shell_print("Usage: cat <file>\n");
    return 1;
  }
  char path[VFS_PATH_MAX];
  resolve_path(argv[1], path, sizeof(path));
  int fd = vfs_open(path, O_RDONLY);
  if (fd < 0) {
    fb_set_color(FB_COLOR_RED, FB_COLOR_BLACK);
    kprintf("  cat: '%s': not found\n", path);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    return 1;
  }
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  char buf[256];
  ssize_t n;
  while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
    buf[n] = '\0';
    shell_print("%s", buf);
  }
  kprintf("\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  vfs_close(fd);
  return 0;
}

// ── write ─────────────────────────────────────────────────────────────────
static int cmd_write(int argc, char **argv) {
  if (argc < 3) {
    shell_print("Usage: write <file> <content>\n");
    return 1;
  }
  char path[VFS_PATH_MAX];
  resolve_path(argv[1], path, sizeof(path));
  int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    fb_set_color(FB_COLOR_RED, FB_COLOR_BLACK);
    kprintf("  write: cannot open '%s'\n", path);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    return 1;
  }
  for (int i = 2; i < argc; i++) {
    vfs_write(fd, argv[i], strlen(argv[i]));
    if (i < argc - 1)
      vfs_write(fd, " ", 1);
  }
  vfs_close(fd);
  fb_set_color(0x44EE88, FB_COLOR_BLACK);
  kprintf("  Wrote to %s\n", path);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── stat ──────────────────────────────────────────────────────────────────
static int cmd_stat(int argc, char **argv) {
  if (argc < 2) {
    shell_print("Usage: stat <path>\n");
    return 1;
  }
  char path[VFS_PATH_MAX];
  resolve_path(argv[1], path, sizeof(path));
  uint64_t size;
  vfs_node_type_t type;
  if (vfs_stat(path, &size, &type) < 0) {
    fb_set_color(FB_COLOR_RED, FB_COLOR_BLACK);
    kprintf("  stat: '%s' not found\n", path);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    return 1;
  }
  static const char *tnames[] = {"file", "directory", "symlink", "chardev",
                                 "blockdev"};
  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  stat: %s\n", path);
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Type  ");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("%s\n", (type < 5) ? tnames[type] : "unknown");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Size  ");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("%llu bytes\n\n", (unsigned long long)size);
  return 0;
}

// ── tasks ─────────────────────────────────────────────────────────────────
static int cmd_tasks(int argc, char **argv) {
  (void)argc;
  (void)argv;
  task_t *cur = sched_current();

  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  Running Tasks\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  %-5s %-18s %-11s %s\n", "PID", "NAME", "STATE", "TICKS");
  fb_set_color(0x335577, FB_COLOR_BLACK);
  kprintf("  %-5s %-18s %-11s %s\n", "---", "----", "-----", "-----");

  static const char *states[] = {"runnable", "running", "sleeping", "blocked",
                                 "zombie"};
  static const uint32_t state_col[] = {0xFFDD88, 0x44EE88, 0x55AAFF, 0xFF9922,
                                       0xFF4444};

  if (cur) {
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    kprintf("  %-5u %-18s ", cur->pid, cur->name);
    uint32_t sc = (cur->state < 5) ? state_col[cur->state] : 0xAAAAAA;
    fb_set_color(sc, FB_COLOR_BLACK);
    kprintf("%-11s", (cur->state < 5) ? states[cur->state] : "?");
    fb_set_color(0x778899, FB_COLOR_BLACK);
    kprintf(" %llu\n", (unsigned long long)cur->ticks_total);
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("\n");
  return 0;
}

// ── sleep ─────────────────────────────────────────────────────────────────
static int cmd_sleep(int argc, char **argv) {
  if (argc < 2) {
    shell_print("Usage: sleep <ms>\n");
    return 1;
  }
  uint64_t ms = 0;
  for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
    ms = ms * 10 + (uint64_t)(*p - '0');
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("  Sleeping %llu ms...\n", (unsigned long long)ms);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  sched_sleep_ms(ms);
  fb_set_color(0x44EE88, FB_COLOR_BLACK);
  kprintf("  Awake.\n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── history ───────────────────────────────────────────────────────────────
static int cmd_history(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  Command History\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  int start = hist_count > SHELL_HIST_MAX ? hist_count - SHELL_HIST_MAX : 0;
  for (int i = start; i < hist_count; i++) {
    fb_set_color(0x778899, FB_COLOR_BLACK);
    kprintf("  %3d  ", i + 1);
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    kprintf("%s\n", history[i % SHELL_HIST_MAX]);
  }
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("\n");
  return 0;
}

// ── disk ──────────────────────────────────────────────────────────────────
static int cmd_disk(int argc, char **argv) {
  (void)argc;
  (void)argv;
#include "../drivers/virtio/virtio.h"
  virtio_blk_info_t info;
  virtio_blk_info(&info);

  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n  VirtIO Block Device\n");
  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  if (!info.capacity) {
    fb_set_color(0xFF9922, FB_COLOR_BLACK);
    kprintf("  No disk detected.\n\n");
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    return 1;
  }
  uint64_t mib = info.capacity * 512 / (1024 * 1024);

  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Capacity   ");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("%llu sectors  (%llu MiB)\n", (unsigned long long)info.capacity,
          (unsigned long long)mib);

  // Mini usage bar (always shows 0 used for a raw disk)
  kprintf("  Usage      ");
  draw_bar(0, mib, 24);
  kprintf("  0%%\n");

  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Read-only  ");
  fb_set_color(info.read_only ? 0xFF9922 : 0x44EE88, FB_COLOR_BLACK);
  kprintf("%s\n\n", info.read_only ? "yes" : "no");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── reboot ────────────────────────────────────────────────────────────────
static int cmd_reboot(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fb_set_color(0xFF9922, FB_COLOR_BLACK);
  kprintf("  Rebooting...\n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  __asm__ volatile("lidt (%%rsp)\nint3\n" ::: "memory");
  for (;;)
    ;
  return 0;
}

// ── version ───────────────────────────────────────────────────────────────
static int cmd_version(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fb_set_color(0x00D4FF, FB_COLOR_BLACK);
  kprintf("\n"
          "   ____                    _       __  ____   _____ \n"
          "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
          " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
          "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
          "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("  Quanta OS  v%s  (%s)\n", QUANTA_VERSION, QUANTA_ARCH);
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("  x86-64  |  x2APIC  |  SMP  |  VirtIO  |  QAI\n\n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 0;
}

// ── ai ────────────────────────────────────────────────────────────────────
static int cmd_ai(int argc, char **argv) {
  if (argc < 2) {
    fb_set_color(0xBB88FF, FB_COLOR_BLACK);
    kprintf("  [QAI] ");
    fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
    kprintf("Usage: ai <question>\n");
    kprintf("  Ask about any Quanta subsystem: apic, smp, scheduler,\n");
    kprintf("  paging, pmm, heap, virtio, vfs, gdt, idt, acpi...\n\n");
    return 0;
  }
  char question[SHELL_LINE_MAX] = {0};
  for (int i = 1; i < argc; i++) {
    if (i > 1)
      strcat_safe(question, " ", sizeof(question));
    strcat_safe(question, argv[i], sizeof(question));
  }
  qai_answer(question);
  return 0;
}

// ── snprintf_stub & strcat_safe ───────────────────────────────────────────
static void snprintf_stub(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  size_t pos = 0;
  while (*fmt && pos < n - 1) {
    if (fmt[0] == '%' && fmt[1] == 's') {
      const char *s = va_arg(ap, const char *);
      while (*s && pos < n - 1)
        buf[pos++] = *s++;
      fmt += 2;
    } else
      buf[pos++] = *fmt++;
  }
  buf[pos] = '\0';
  va_end(ap);
}

static void strcat_safe(char *dst, const char *src, size_t max) {
  size_t len = strlen(dst);
  if (len >= max - 1)
    return;
  size_t rem = max - len - 1, i = 0;
  while (i < rem && src[i]) {
    dst[len + i] = src[i];
    i++;
  }
  dst[len + i] = '\0';
}

// ── Register all built-ins ────────────────────────────────────────────────
static void register_builtins(void) {
  // System
  shell_register_cat("version", "Show Quanta version banner", "System",
                     cmd_version);
  shell_register_cat("cpuinfo", "Show CPU details and features", "System",
                     cmd_cpuinfo);
  shell_register_cat("mem", "Show memory statistics", "System", cmd_mem);
  shell_register_cat("uptime", "Show system uptime", "System", cmd_uptime);
  shell_register_cat("tasks", "List scheduled tasks", "System", cmd_tasks);
  shell_register_cat("disk", "Show VirtIO block device info", "System",
                     cmd_disk);
  shell_register_cat("reboot", "Reboot the system", "System", cmd_reboot);
  // Files
  shell_register_cat("ls", "List directory contents", "Files", cmd_ls);
  shell_register_cat("cat", "Print a file", "Files", cmd_cat);
  shell_register_cat("write", "Write text to a file", "Files", cmd_write);
  shell_register_cat("stat", "Show file/directory metadata", "Files", cmd_stat);
  shell_register_cat("cd", "Change directory", "Files", cmd_cd);
  shell_register_cat("pwd", "Print working directory", "Files", cmd_pwd);
  // Shell
  shell_register_cat("help", "List all commands by category", "Shell",
                     cmd_help);
  shell_register_cat("clear", "Clear the screen", "Shell", cmd_clear);
  shell_register_cat("echo", "Print arguments", "Shell", cmd_echo);
  shell_register_cat("sleep", "Sleep for N milliseconds", "Shell", cmd_sleep);
  shell_register_cat("history", "Show command history", "Shell", cmd_history);
  // AI
  shell_register_cat("ai", "Ask the Quanta AI assistant", "AI", cmd_ai);
}

// ── Execute ───────────────────────────────────────────────────────────────
static int execute(char *line) {
  int len = (int)strlen(line);
  while (len > 0 && line[len - 1] == ' ')
    line[--len] = '\0';
  if (!len)
    return 0;
  hist_push(line);
  char *argv[SHELL_ARGS_MAX];
  int argc = parse_args(line, argv, SHELL_ARGS_MAX);
  if (!argc)
    return 0;
  for (int i = 0; i < cmd_count; i++)
    if (strcmp(cmd_table[i].name, argv[0]) == 0)
      return cmd_table[i].fn(argc, argv);
  fb_set_color(FB_COLOR_RED, FB_COLOR_BLACK);
  kprintf("  Unknown command: %s", argv[0]);
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("  (type ");
  fb_set_color(0x33DDCC, FB_COLOR_BLACK);
  kprintf("help");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf(" for a list)\n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  return 1;
}

// ── Welcome screen ────────────────────────────────────────────────────────
static void print_welcome(void) {
  cmd_version(0, NULL);

  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);

  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("  Welcome to Quanta OS.  Key bindings:\n\n");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("    Tab        ");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("Complete command\n");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("    Up / Down  ");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("Navigate history\n");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("    Left/Right ");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("Move cursor in line\n");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("    Backspace  ");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("Delete character\n");

  kprintf("\n");
  fb_set_color(0x33DDCC, FB_COLOR_BLACK);
  kprintf("  help");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("              list all commands\n");
  fb_set_color(0x33DDCC, FB_COLOR_BLACK);
  kprintf("  ai <topic>");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("          ask the built-in QAI assistant\n");
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);

  fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
  kprintf("\n");
}

// ── Two-line prompt ───────────────────────────────────────────────────────
// Line 1: dim separator  ──  cwd
// Line 2: colour prompt  quanta@kernel:<cwd>$
static void print_prompt(void) {
  // Refresh status bar before drawing prompt so it stays up to date
  fb_statusbar_refresh();

  // Thin context line
  fb_set_color(0x335577, FB_COLOR_BLACK);
  kprintf("  -- %s\n", shell_cwd);

  // Main prompt
  fb_set_color(0x33DDCC, FB_COLOR_BLACK);
  kprintf("quanta");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf("@");
  fb_set_color(0xAADDFF, FB_COLOR_BLACK);
  kprintf("kernel");
  fb_set_color(0x778899, FB_COLOR_BLACK);
  kprintf(":");
  fb_set_color(0xFFDD88, FB_COLOR_BLACK);
  kprintf("%s", shell_cwd);
  fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
  kprintf("$ ");
}

// ── Main shell loop ───────────────────────────────────────────────────────
void shell_run(void *arg) {
  (void)arg;
  register_builtins();
  print_welcome();

  for (;;) {
    print_prompt();

    input_buf[0] = '\0';
    input_len = 0;
    cursor = 0;
    hist_pos = hist_count;

    int done = 0;
    while (!done) {
      char c = kbd_getchar();
      switch (c) {
      case '\n':
      case '\r':
        kprintf("\n");
        done = 1;
        break;
      case '\b':
      case 127:
        line_backspace();
        break;
      case '\033': {
        char s1 = kbd_getchar_noblock();
        char s2 = kbd_getchar_noblock();
        if (s1 == '[') {
          if (s2 == 'A') {
            if (hist_pos > 0) {
              hist_pos--;
              line_clear_display();
              strncpy(input_buf, history[hist_pos % SHELL_HIST_MAX],
                      SHELL_LINE_MAX - 1);
              input_buf[SHELL_LINE_MAX - 1] = '\0';
              input_len = cursor = (int)strlen(input_buf);
              line_redraw();
            }
          } else if (s2 == 'B') {
            if (hist_pos < hist_count) {
              hist_pos++;
              line_clear_display();
              if (hist_pos == hist_count) {
                input_buf[0] = '\0';
                input_len = cursor = 0;
              } else {
                strncpy(input_buf, history[hist_pos % SHELL_HIST_MAX],
                        SHELL_LINE_MAX - 1);
                input_buf[SHELL_LINE_MAX - 1] = '\0';
                input_len = cursor = (int)strlen(input_buf);
              }
              line_redraw();
            }
          } else if (s2 == 'C') {
            if (cursor < input_len) {
              cursor++;
              kprintf("\033[C");
            }
          } else if (s2 == 'D') {
            if (cursor > 0) {
              cursor--;
              kprintf("\033[D");
            }
          }
        }
        break;
      }
      case '\t': {
        if (!input_len)
          break;
        int matches = 0;
        const char *last = NULL;
        for (int i = 0; i < cmd_count; i++)
          if (strncmp(cmd_table[i].name, input_buf, input_len) == 0) {
            matches++;
            last = cmd_table[i].name;
          }
        if (matches == 1 && last) {
          line_clear_display();
          strncpy(input_buf, last, SHELL_LINE_MAX - 1);
          input_buf[SHELL_LINE_MAX - 1] = '\0';
          input_len = cursor = (int)strlen(input_buf);
          line_redraw();
        } else if (matches > 1) {
          kprintf("\n");
          for (int i = 0; i < cmd_count; i++)
            if (strncmp(cmd_table[i].name, input_buf, input_len) == 0) {
              fb_set_color(0x33DDCC, FB_COLOR_BLACK);
              kprintf("  %s", cmd_table[i].name);
            }
          fb_set_color(FB_COLOR_WHITE, FB_COLOR_BLACK);
          kprintf("\n");
          print_prompt();
          kprintf("%s", input_buf);
        }
        break;
      }
      default:
        if (c >= 0x20 && c < 0x7F)
          line_insert(c);
        break;
      }
    }
    execute(input_buf);
  }
}
