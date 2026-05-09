// ============================================================
//  shell/shell.c — Quanta AI Interactive Shell (QAI)  v2.1
//
//  Foundation fixes:
//    • Added cd / pwd commands (users naturally try them)
//    • Tracks current working directory properly
//    • All Unicode box-drawing replaced with ASCII
//    • cmd_ls resolves relative paths against CWD
//    • cmd_write handles spaces via quoted strings
//    • snprintf_stub hardened against overflow
// ============================================================

#include "shell.h"
#include "qai.h"
#include "../drivers/framebuffer.h"
#include "../drivers/serial.h"
#include "../drivers/keyboard.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "../cpu/cpuid.h"
#include "../boot/limine_requests.h"
#include <stdarg.h>
#include <stddef.h>

// ── Forward declarations for utility helpers ──────────────────────────────
static void snprintf_stub(char *buf, size_t n, const char *fmt, ...);
static void strcat_safe(char *dst, const char *src, size_t max);

// ── Command registry ──────────────────────────────────────────────────────
#define MAX_CMDS 48

typedef struct {
    char          name[32];
    char          help[80];
    shell_cmd_fn  fn;
} cmd_entry_t;

static cmd_entry_t  cmd_table[MAX_CMDS];
static int          cmd_count = 0;

// ── Current working directory ─────────────────────────────────────────────
static char shell_cwd[VFS_PATH_MAX] = "/";

static void shell_set_cwd(const char *path) {
    strncpy(shell_cwd, path, VFS_PATH_MAX - 1);
    shell_cwd[VFS_PATH_MAX - 1] = '\0';
}

// Resolve a path: if relative, prepend CWD
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
    // Relative: cwd + / + in
    size_t clen = strlen(shell_cwd);
    size_t ilen = strlen(in);
    if (clen + 1 + ilen >= outsz) {
        // too long, fallback
        strncpy(out, in, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    memcpy(out, shell_cwd, clen);
    if (shell_cwd[clen - 1] != '/') {
        out[clen] = '/';
        clen++;
    }
    memcpy(out + clen, in, ilen + 1);
}

void shell_register(const char *name, const char *help, shell_cmd_fn fn) {
    if (cmd_count >= MAX_CMDS) return;
    strncpy(cmd_table[cmd_count].name, name, 31);
    cmd_table[cmd_count].name[31] = '\0';
    strncpy(cmd_table[cmd_count].help, help, 79);
    cmd_table[cmd_count].help[79] = '\0';
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
static char  history[SHELL_HIST_MAX][SHELL_LINE_MAX];
static int   hist_count = 0;
static int   hist_pos   = 0;

static void hist_push(const char *line) {
    if (!*line) return;
    if (hist_count > 0 &&
        strcmp(history[(hist_count - 1) % SHELL_HIST_MAX], line) == 0) return;
    strncpy(history[hist_count % SHELL_HIST_MAX], line, SHELL_LINE_MAX - 1);
    history[hist_count % SHELL_HIST_MAX][SHELL_LINE_MAX - 1] = '\0';
    hist_count++;
    hist_pos = hist_count;
}

// ── Line editor ───────────────────────────────────────────────────────────
static char   input_buf[SHELL_LINE_MAX];
static int    input_len = 0;
static int    cursor    = 0;

static void line_clear_display(void) {
    for (int i = 0; i < cursor; i++) kprintf("\b");
    for (int i = 0; i < input_len; i++) kprintf(" ");
    for (int i = 0; i < input_len; i++) kprintf("\b");
}

static void line_redraw(void) {
    kprintf("%s", input_buf);
    for (int i = input_len; i > cursor; i--) kprintf("\b");
}

static void line_insert(char c) {
    if (input_len >= SHELL_LINE_MAX - 1) return;
    for (int i = input_len; i > cursor; i--)
        input_buf[i] = input_buf[i-1];
    input_buf[cursor++] = c;
    input_buf[++input_len] = '\0';
    kprintf("%s", input_buf + cursor - 1);
    for (int i = input_len; i > cursor; i--) kprintf("\b");
}

static void line_backspace(void) {
    if (!cursor) return;
    for (int i = cursor - 1; i < input_len - 1; i++)
        input_buf[i] = input_buf[i+1];
    input_buf[--input_len] = '\0';
    cursor--;
    kprintf("\b");
    kprintf("%s ", input_buf + cursor);
    for (int i = input_len + 1; i > cursor; i--) kprintf("\b");
}

// ── Command parsing ───────────────────────────────────────────────────────
static int parse_args(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        // Handle quoted strings
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

// ── Built-in commands ─────────────────────────────────────────────────────

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_print(ANSI_CYAN "\nQuanta OS -- Available Commands\n" ANSI_RESET);
    shell_print(ANSI_GRAY "-----------------------------------------------\n" ANSI_RESET);
    for (int i = 0; i < cmd_count; i++) {
        shell_print("  " ANSI_GREEN "%-14s" ANSI_RESET " %s\n",
                    cmd_table[i].name, cmd_table[i].help);
    }
    shell_print("\n");
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    fb_clear();
    return 0;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        shell_print("%s%s", argv[i], (i < argc-1) ? " " : "");
    }
    shell_print("\n");
    return 0;
}

static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_print("%s\n", shell_cwd);
    return 0;
}

static int cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        shell_set_cwd("/");
        return 0;
    }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1], path, sizeof(path));

    uint64_t size; vfs_node_type_t type;
    if (vfs_stat(path, &size, &type) < 0 || type != VFS_DIR) {
        shell_print(ANSI_RED "cd: '%s' is not a directory\n" ANSI_RESET, argv[1]);
        return 1;
    }
    shell_set_cwd(path);
    return 0;
}

static int cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    pmm_stats();
    heap_stats();
    return 0;
}

static int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t ms  = sched_uptime_ms();
    uint64_t sec = ms / 1000;
    uint64_t min = sec / 60;
    uint64_t hr  = min / 60;
    shell_print("Uptime: %lluh %llum %llus (%llu ms)\n",
                (unsigned long long)hr,
                (unsigned long long)(min % 60),
                (unsigned long long)(sec % 60),
                (unsigned long long)ms);
    return 0;
}

static int cmd_cpuinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char vendor[13], brand[49];
    cpu_vendor(vendor);
    cpu_brand(brand);

    struct limine_smp_response *smp = limine_smp();
    uint32_t ncpus = smp ? (uint32_t)smp->cpu_count : 1;

    shell_print(ANSI_CYAN "CPU Information\n" ANSI_RESET);
    shell_print("  Vendor  : %s\n", vendor);
    shell_print("  Brand   : %s\n", brand);
    shell_print("  CPUs    : %u\n", ncpus);
    shell_print("  x2APIC  : %s\n", cpu_has_x2apic() ? "yes" : "no");
    shell_print("  TSC     : %s\n", cpu_has_tsc() ? "yes" : "no");
    shell_print("  NX      : %s\n", cpu_has_nx() ? "yes" : "no");
    shell_print("  Inv.TSC : %s\n", cpu_has_invariant_tsc() ? "yes" : "no");
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    char path[VFS_PATH_MAX];
    resolve_path((argc > 1) ? argv[1] : ".", path, sizeof(path));

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) { shell_print("ls: cannot open '%s'\n", path); return 1; }

    char name[VFS_NAME_MAX];
    uint32_t idx = 0;
    uint64_t size; vfs_node_type_t type;
    shell_print(ANSI_CYAN "%s" ANSI_RESET ":\n", path);
    while (vfs_readdir(fd, idx++, name) == 0) {
        char full[VFS_PATH_MAX];
        size_t plen = strlen(path);
        if (plen > 0 && path[plen-1] == '/')
            snprintf_stub(full, sizeof(full), "%s%s", path, name);
        else
            snprintf_stub(full, sizeof(full), "%s/%s", path, name);

        if (vfs_stat(full, &size, &type) < 0) { size = 0; type = VFS_FILE; }
        const char *color = (type == VFS_DIR) ? ANSI_BLUE :
                            (type == VFS_CHARDEV || type == VFS_BLOCKDEV) ? ANSI_YELLOW :
                            ANSI_WHITE;
        const char *suffix = (type == VFS_DIR) ? "/" : "";
        shell_print("  %s%s%s" ANSI_RESET, color, name, suffix);
        if (type == VFS_FILE && size > 0)
            shell_print(ANSI_GRAY "  (%llu B)" ANSI_RESET, (unsigned long long)size);
        shell_print("\n");
    }
    vfs_close(fd);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { shell_print("Usage: cat <file>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1], path, sizeof(path));

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) { shell_print("cat: cannot open '%s'\n", path); return 1; }
    char buf[256];
    ssize_t n;
    while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        shell_print("%s", buf);
    }
    shell_print("\n");
    vfs_close(fd);
    return 0;
}

static int cmd_write(int argc, char **argv) {
    if (argc < 3) { shell_print("Usage: write <file> <content>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1], path, sizeof(path));

    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { shell_print("write: cannot open '%s'\n", path); return 1; }
    // Join remaining args with spaces (quoted strings handled by parser)
    for (int i = 2; i < argc; i++) {
        vfs_write(fd, argv[i], strlen(argv[i]));
        if (i < argc - 1) vfs_write(fd, " ", 1);
    }
    vfs_close(fd);
    shell_print("Written to %s\n", path);
    return 0;
}

static int cmd_tasks(int argc, char **argv) {
    (void)argc; (void)argv;
    task_t *cur = sched_current();
    shell_print(ANSI_CYAN "%-5s %-18s %-10s\n" ANSI_RESET,
                "PID", "NAME", "STATE");
    shell_print(ANSI_GRAY "---------------------------------\n" ANSI_RESET);
    static const char *states[] = { "runnable","running","sleeping","blocked","zombie" };
    if (cur) {
        shell_print("%-5u %-18s %-10s  ticks=%llu\n",
            cur->pid, cur->name,
            (cur->state < 5) ? states[cur->state] : "?",
            (unsigned long long)cur->ticks_total);
    }
    return 0;
}

static int cmd_sleep(int argc, char **argv) {
    if (argc < 2) { shell_print("Usage: sleep <ms>\n"); return 1; }
    uint64_t ms = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
        ms = ms * 10 + (uint64_t)(*p - '0');
    shell_print("Sleeping %llu ms...\n", (unsigned long long)ms);
    sched_sleep_ms(ms);
    shell_print("Awake.\n");
    return 0;
}

static int cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    int start = hist_count > SHELL_HIST_MAX ? hist_count - SHELL_HIST_MAX : 0;
    for (int i = start; i < hist_count; i++) {
        shell_print("  %3d  %s\n", i + 1,
                    history[i % SHELL_HIST_MAX]);
    }
    return 0;
}

static int cmd_disk(int argc, char **argv) {
    (void)argc; (void)argv;
    #include "../drivers/virtio/virtio.h"
    virtio_blk_info_t info;
    virtio_blk_info(&info);
    if (!info.capacity) {
        shell_print("No disk detected.\n");
        return 1;
    }
    shell_print("VirtIO Block Device:\n");
    shell_print("  Capacity : %llu sectors  (%llu MiB)\n",
                (unsigned long long)info.capacity,
                (unsigned long long)(info.capacity * 512 / (1024*1024)));
    shell_print("  Read-only: %s\n", info.read_only ? "yes" : "no");
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_print(ANSI_YELLOW "Rebooting...\n" ANSI_RESET);
    __asm__ volatile (
        "lidt (%%rsp)\n"
        "int3\n"
        ::: "memory"
    );
    for(;;);
    return 0;
}

static int cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    shell_print(ANSI_CYAN
        "   ____                    _       __  ____   _____ \n"
        "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
        " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
        "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
        "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n"
        ANSI_RESET);
    shell_print(ANSI_BOLD "  Quanta OS  v%s  (%s)\n" ANSI_RESET,
                QUANTA_VERSION, QUANTA_ARCH);
    shell_print(ANSI_GRAY
        "  x86-64 kernel  |  x2APIC  |  SMP  |  VirtIO  |  QAI\n"
        ANSI_RESET "\n");
    return 0;
}

static int cmd_ai(int argc, char **argv) {
    if (argc < 2) {
        shell_print(ANSI_MAGENTA "[QAI] " ANSI_RESET
                    "Usage: ai <question>\n"
                    "  Ask the Quanta AI assistant anything about\n"
                    "  the kernel, OS concepts, or system status.\n");
        return 0;
    }
    char question[SHELL_LINE_MAX] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat_safe(question, " ", sizeof(question));
        strcat_safe(question, argv[i], sizeof(question));
    }
    qai_answer(question);
    return 0;
}

static int cmd_stat(int argc, char **argv) {
    if (argc < 2) { shell_print("Usage: stat <path>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1], path, sizeof(path));

    uint64_t size; vfs_node_type_t type;
    if (vfs_stat(path, &size, &type) < 0) {
        shell_print("stat: '%s' not found\n", path); return 1;
    }
    static const char *tnames[] = {"file","directory","symlink","chardev","blockdev"};
    shell_print("  Path : %s\n", path);
    shell_print("  Type : %s\n", (type < 5) ? tnames[type] : "unknown");
    shell_print("  Size : %llu bytes\n", (unsigned long long)size);
    return 0;
}

// ── snprintf_stub — minimal but safe ──────────────────────────────────────
static void snprintf_stub(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    size_t pos = 0;
    while (*fmt && pos < n - 1) {
        if (fmt[0] == '%' && fmt[1] == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s && pos < n - 1) buf[pos++] = *s++;
            fmt += 2;
        } else {
            buf[pos++] = *fmt++;
        }
    }
    buf[pos] = '\0';
    va_end(ap);
}

// ── strcat_safe ───────────────────────────────────────────────────────────
static void strcat_safe(char *dst, const char *src, size_t max) {
    size_t len = strlen(dst);
    if (len >= max - 1) return;
    size_t rem = max - len - 1;
    size_t i = 0;
    while (i < rem && src[i]) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = '\0';
}

// ── Register all built-ins ────────────────────────────────────────────────
static void register_builtins(void) {
    shell_register("help",    "List all commands",                 cmd_help);
    shell_register("clear",   "Clear the screen",                  cmd_clear);
    shell_register("echo",    "Print arguments",                   cmd_echo);
    shell_register("version", "Show Quanta version banner",        cmd_version);
    shell_register("mem",     "Show memory statistics",            cmd_mem);
    shell_register("cpuinfo", "Show CPU information",              cmd_cpuinfo);
    shell_register("uptime",  "Show system uptime",                cmd_uptime);
    shell_register("tasks",   "List running tasks",                cmd_tasks);
    shell_register("ls",      "List directory contents",           cmd_ls);
    shell_register("cat",     "Print a file",                      cmd_cat);
    shell_register("write",   "Write text to a file",              cmd_write);
    shell_register("stat",    "Show file/directory metadata",      cmd_stat);
    shell_register("sleep",   "Sleep for N milliseconds",          cmd_sleep);
    shell_register("disk",    "Show VirtIO block device info",     cmd_disk);
    shell_register("history", "Show command history",              cmd_history);
    shell_register("reboot",  "Reboot the system",                 cmd_reboot);
    shell_register("ai",      "Ask the Quanta AI assistant",       cmd_ai);
    shell_register("cd",      "Change directory",                  cmd_cd);
    shell_register("pwd",     "Print working directory",           cmd_pwd);
}

// ── Execute a command line ────────────────────────────────────────────────
static int execute(char *line) {
    int len = (int)strlen(line);
    while (len > 0 && line[len-1] == ' ') line[--len] = '\0';
    if (!len) return 0;

    hist_push(line);

    char *argv[SHELL_ARGS_MAX];
    int   argc = parse_args(line, argv, SHELL_ARGS_MAX);
    if (!argc) return 0;

    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, argv[0]) == 0) {
            return cmd_table[i].fn(argc, argv);
        }
    }

    shell_print(ANSI_RED "Unknown command: %s" ANSI_RESET
                "  (type " ANSI_CYAN "help" ANSI_RESET " for a list)\n",
                argv[0]);
    return 1;
}

// ── Main shell loop ───────────────────────────────────────────────────────
void shell_run(void *arg) {
    (void)arg;
    register_builtins();

    cmd_version(0, NULL);
    shell_print(ANSI_GRAY
        "Type " ANSI_CYAN "help" ANSI_GRAY " for available commands.\n"
        "Type " ANSI_MAGENTA "ai <question>" ANSI_GRAY
        " to talk to the QAI assistant.\n\n" ANSI_RESET);

    for (;;) {
        shell_print(SHELL_PROMPT);
        input_buf[0] = '\0';
        input_len    = 0;
        cursor       = 0;
        hist_pos     = hist_count;

        bool done = false;
        while (!done) {
            char c = kbd_getchar();

            switch (c) {
                case '\n':
                case '\r':
                    shell_print("\n");
                    done = true;
                    break;

                case '\b':
                case 127:
                    line_backspace();
                    break;

                case '\033': {
                    char seq1 = kbd_getchar_noblock();
                    char seq2 = kbd_getchar_noblock();
                    if (seq1 == '[') {
                        if (seq2 == 'A') {
                            if (hist_pos > 0) {
                                hist_pos--;
                                line_clear_display();
                                strncpy(input_buf,
                                        history[hist_pos % SHELL_HIST_MAX],
                                        SHELL_LINE_MAX - 1);
                                input_buf[SHELL_LINE_MAX-1] = '\0';
                                input_len = cursor = (int)strlen(input_buf);
                                line_redraw();
                            }
                        } else if (seq2 == 'B') {
                            if (hist_pos < hist_count) {
                                hist_pos++;
                                line_clear_display();
                                if (hist_pos == hist_count) {
                                    input_buf[0] = '\0';
                                    input_len = cursor = 0;
                                } else {
                                    strncpy(input_buf,
                                            history[hist_pos % SHELL_HIST_MAX],
                                            SHELL_LINE_MAX - 1);
                                    input_buf[SHELL_LINE_MAX-1] = '\0';
                                    input_len = cursor = (int)strlen(input_buf);
                                }
                                line_redraw();
                            }
                        } else if (seq2 == 'C') {
                            if (cursor < input_len) { cursor++; kprintf("\033[C"); }
                        } else if (seq2 == 'D') {
                            if (cursor > 0) { cursor--; kprintf("\033[D"); }
                        }
                    }
                    break;
                }

                case '\t': {
                    if (!input_len) break;
                    int matches = 0;
                    const char *last_match = NULL;
                    for (int i = 0; i < cmd_count; i++) {
                        if (strncmp(cmd_table[i].name, input_buf, input_len) == 0) {
                            matches++;
                            last_match = cmd_table[i].name;
                        }
                    }
                    if (matches == 1 && last_match) {
                        line_clear_display();
                        strncpy(input_buf, last_match, SHELL_LINE_MAX - 1);
                        input_buf[SHELL_LINE_MAX-1] = '\0';
                        input_len = cursor = (int)strlen(input_buf);
                        line_redraw();
                    } else if (matches > 1) {
                        shell_print("\n");
                        for (int i = 0; i < cmd_count; i++) {
                            if (strncmp(cmd_table[i].name, input_buf, input_len) == 0)
                                shell_print(ANSI_CYAN "%s  " ANSI_RESET,
                                            cmd_table[i].name);
                        }
                        shell_print("\n" SHELL_PROMPT "%s", input_buf);
                    }
                    break;
                }

                default:
                    if (c >= 0x20 && c < 0x7F) {
                        line_insert(c);
                    }
                    break;
            }
        }

        execute(input_buf);
    }
}
