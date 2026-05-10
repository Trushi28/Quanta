// ============================================================
//  shell/shell.c — Quanta Interactive Shell  (Phase 3)
//
//  New commands:
//    mkdir <path>           — create directory
//    rm [-r] <path>         — remove file or directory tree
//    mv <src> <dst>         — rename / move
//    cp <src> <dst>         — copy a file
//    touch <file>           — create empty file or update mtime
//    chmod <octal> <path>   — change permissions
//    top                    — show ALL tasks with tick counts
//    free                   — visual physical-memory bar
//    hexdump <file>         — hex+ASCII dump (first 512 bytes)
//    kv <sub> [args]        — persistent key-value store
//
//  Improved commands:
//    ls [-l] [path]         — long listing with metadata
//    tasks                  — now shows all tasks (not just current)
//    stat <path>            — now shows full inode metadata
//    mem                    — now shows visual bar + heap breakdown
// ============================================================
#include "shell.h"
#include "qai.h"
#include "../boot/limine_requests.h"
#include "../cpu/cpuid.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/kvstore.h"
#include "../drivers/virtio/virtio.h"
#include "../fs/vfs.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"
#include <stdarg.h>
#include <stddef.h>

// ── Forward declarations ──────────────────────────────────────────────────
static void strcat_safe(char *dst, const char *src, size_t max);
static void snprintf_stub(char *buf, size_t n, const char *fmt, ...);
static void fmt_mode(uint16_t mode, vfs_node_type_t type, char out[11]);
static void fmt_uptime_ms(uint64_t ms, char out[24]);
static void draw_bar(uint64_t used, uint64_t total, int width);

// ── Command registry ──────────────────────────────────────────────────────
#define MAX_CMDS 64

typedef struct {
    char          name[32];
    char          help[88];
    const char   *category;
    shell_cmd_fn  fn;
} cmd_entry_t;

static cmd_entry_t cmd_table[MAX_CMDS];
static int         cmd_count = 0;

// ── CWD ───────────────────────────────────────────────────────────────────
static char shell_cwd[VFS_PATH_MAX] = "/";

static void shell_set_cwd(const char *p) {
    strncpy(shell_cwd, p, VFS_PATH_MAX - 1);
    shell_cwd[VFS_PATH_MAX - 1] = '\0';
}

static void resolve_path(const char *in, char *out, size_t outsz) {
    if (!in || !*in) { strncpy(out, shell_cwd, outsz - 1); out[outsz-1]='\0'; return; }
    if (in[0] == '/') { strncpy(out, in, outsz - 1); out[outsz-1]='\0'; return; }
    size_t cl = strlen(shell_cwd), il = strlen(in);
    if (cl + 1 + il >= outsz) { strncpy(out, in, outsz-1); out[outsz-1]='\0'; return; }
    memcpy(out, shell_cwd, cl);
    if (shell_cwd[cl-1] != '/') out[cl++] = '/';
    memcpy(out+cl, in, il+1);
}

// ── Registration ──────────────────────────────────────────────────────────
void shell_register(const char *name, const char *help, shell_cmd_fn fn) {
    if (cmd_count >= MAX_CMDS) return;
    strncpy(cmd_table[cmd_count].name, name, 31); cmd_table[cmd_count].name[31]='\0';
    strncpy(cmd_table[cmd_count].help, help, 87); cmd_table[cmd_count].help[87]='\0';
    cmd_table[cmd_count].category = "Shell";
    cmd_table[cmd_count].fn = fn;
    cmd_count++;
}

static void shell_register_cat(const char *name, const char *help,
                                const char *cat, shell_cmd_fn fn) {
    if (cmd_count >= MAX_CMDS) return;
    strncpy(cmd_table[cmd_count].name, name, 31); cmd_table[cmd_count].name[31]='\0';
    strncpy(cmd_table[cmd_count].help, help, 87); cmd_table[cmd_count].help[87]='\0';
    cmd_table[cmd_count].category = cat;
    cmd_table[cmd_count].fn = fn;
    cmd_count++;
}

void shell_print(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

// ── History ───────────────────────────────────────────────────────────────
static char history[SHELL_HIST_MAX][SHELL_LINE_MAX];
static int  hist_count = 0, hist_pos = 0;

static void hist_push(const char *line) {
    if (!*line) return;
    if (hist_count > 0 &&
        strcmp(history[(hist_count-1)%SHELL_HIST_MAX], line) == 0) return;
    strncpy(history[hist_count%SHELL_HIST_MAX], line, SHELL_LINE_MAX-1);
    history[hist_count%SHELL_HIST_MAX][SHELL_LINE_MAX-1] = '\0';
    hist_count++;
    hist_pos = hist_count;
}

// ── Line editor ───────────────────────────────────────────────────────────
static char input_buf[SHELL_LINE_MAX];
static int  input_len = 0, cursor = 0;

static void line_clear_display(void) {
    for (int i=0;i<cursor;i++)   kprintf("\b");
    for (int i=0;i<input_len;i++) kprintf(" ");
    for (int i=0;i<input_len;i++) kprintf("\b");
}
static void line_redraw(void) {
    kprintf("%s", input_buf);
    for (int i=input_len; i>cursor; i--) kprintf("\b");
}
static void line_insert(char c) {
    if (input_len >= SHELL_LINE_MAX-1) return;
    for (int i=input_len; i>cursor; i--) input_buf[i]=input_buf[i-1];
    input_buf[cursor++] = c;
    input_buf[++input_len] = '\0';
    kprintf("%s", input_buf+cursor-1);
    for (int i=input_len; i>cursor; i--) kprintf("\b");
}
static void line_backspace(void) {
    if (!cursor) return;
    for (int i=cursor-1; i<input_len-1; i++) input_buf[i]=input_buf[i+1];
    input_buf[--input_len] = '\0';
    cursor--;
    kprintf("\b");
    kprintf("%s ", input_buf+cursor);
    for (int i=input_len+1; i>cursor; i--) kprintf("\b");
}

// ── Argument parser ───────────────────────────────────────────────────────
static int parse_args(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            p++; argv[argc++] = p;
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

// ── Formatting helpers ────────────────────────────────────────────────────

// Format a 10-char Unix mode string (e.g. "-rw-r--r--")
static void fmt_mode(uint16_t mode, vfs_node_type_t type, char out[11]) {
    switch (type) {
        case VFS_DIR:      out[0]='d'; break;
        case VFS_CHARDEV:  out[0]='c'; break;
        case VFS_BLOCKDEV: out[0]='b'; break;
        case VFS_SYMLINK:  out[0]='l'; break;
        default:           out[0]='-'; break;
    }
    out[1]=(mode&0400)?'r':'-'; out[2]=(mode&0200)?'w':'-'; out[3]=(mode&0100)?'x':'-';
    out[4]=(mode&0040)?'r':'-'; out[5]=(mode&0020)?'w':'-'; out[6]=(mode&0010)?'x':'-';
    out[7]=(mode&0004)?'r':'-'; out[8]=(mode&0002)?'w':'-'; out[9]=(mode&0001)?'x':'-';
    out[10]='\0';
}

// Format ms-since-boot as HH:MM:SS
static void fmt_uptime_ms(uint64_t ms, char out[24]) {
    uint64_t s = ms/1000, m = s/60, h = m/60;
    // Simple formatting without sprintf
    int i=0;
    out[i++] = '0'+(char)((h/10)%10); out[i++] = '0'+(char)(h%10);
    out[i++] = ':';
    out[i++] = '0'+(char)((m%60)/10); out[i++] = '0'+(char)(m%60%10);
    out[i++] = ':';
    out[i++] = '0'+(char)((s%60)/10); out[i++] = '0'+(char)(s%60%10);
    out[i]   = '\0';
}

// Draw a [====----] bar (width chars wide)
static void draw_bar(uint64_t used, uint64_t total, int width) {
    if (!total) total=1;
    int filled = (int)((used*(uint64_t)width)/total);
    kprintf("[");
    for (int i=0; i<width; i++) {
        if (i<filled) { fb_set_color(0x44EE88,FB_COLOR_BLACK); kprintf("#"); }
        else           { fb_set_color(0x335577,FB_COLOR_BLACK); kprintf("-"); }
    }
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("]");
}

// Parse an octal string (e.g. "755") to uint16_t
static uint16_t parse_octal(const char *s) {
    uint16_t v = 0;
    while (*s >= '0' && *s <= '7') { v = (uint16_t)(v*8 + (*s-'0')); s++; }
    return v;
}

// ── BUILT-IN COMMANDS ─────────────────────────────────────────────────────

// ── help ──────────────────────────────────────────────────────────────────
static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  Quanta OS  --  Commands\n");
    fb_draw_hline('-', 0x335577, FB_COLOR_BLACK);
    static const char *cats[] = {"System","Files","Shell","AI",NULL};
    for (int ci=0; cats[ci]; ci++) {
        fb_set_color(0xAADDFF,FB_COLOR_BLACK);
        kprintf("\n  [%s]\n", cats[ci]);
        for (int i=0; i<cmd_count; i++) {
            if (strcmp(cmd_table[i].category, cats[ci])!=0) continue;
            fb_set_color(0x33DDCC,FB_COLOR_BLACK);
            kprintf("    %-14s",cmd_table[i].name);
            fb_set_color(0x99AABB,FB_COLOR_BLACK);
            kprintf("%s\n",cmd_table[i].help);
        }
    }
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("\n  Tab=complete  Up/Down=history  Left/Right=cursor\n\n");
    return 0;
}

// ── clear ─────────────────────────────────────────────────────────────────
static int cmd_clear(int argc, char **argv) {
    (void)argc;(void)argv;
    fb_clear(); fb_statusbar_refresh(); return 0;
}

// ── echo ──────────────────────────────────────────────────────────────────
static int cmd_echo(int argc, char **argv) {
    for (int i=1;i<argc;i++) shell_print("%s%s",argv[i],(i<argc-1)?" ":"");
    shell_print("\n"); return 0;
}

// ── pwd ───────────────────────────────────────────────────────────────────
static int cmd_pwd(int argc, char **argv) {
    (void)argc;(void)argv;
    fb_set_color(0x33DDCC,FB_COLOR_BLACK);
    kprintf("%s\n",shell_cwd);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── cd ────────────────────────────────────────────────────────────────────
static int cmd_cd(int argc, char **argv) {
    if (argc<2) { shell_set_cwd("/"); return 0; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    uint64_t sz; vfs_node_type_t t;
    if (vfs_stat(path,&sz,&t)<0 || t!=VFS_DIR) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  cd: '%s': not a directory\n",argv[1]);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    shell_set_cwd(path); return 0;
}

// ── ls ────────────────────────────────────────────────────────────────────
static int cmd_ls(int argc, char **argv) {
    bool long_fmt = false;
    const char *path_arg = NULL;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"-l")==0) long_fmt=true;
        else path_arg = argv[i];
    }
    char path[VFS_PATH_MAX];
    resolve_path(path_arg?path_arg:".",path,sizeof(path));

    int fd = vfs_open(path,O_RDONLY);
    if (fd<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  ls: cannot open '%s'\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }

    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  %s\n",path);
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);

    if (long_fmt) {
        fb_set_color(0x778899,FB_COLOR_BLACK);
        kprintf("  %-10s %2s %6s  %-8s  %s\n","MODE","NL","SIZE","MODIFIED","NAME");
        fb_set_color(0x335577,FB_COLOR_BLACK);
        kprintf("  ---------- -- ------  --------  ----\n");
    }

    char name[VFS_NAME_MAX];
    uint32_t idx=0;
    while (vfs_readdir(fd,idx++,name)==0) {
        char full[VFS_PATH_MAX];
        size_t pl=strlen(path);
        if (pl>0 && path[pl-1]=='/') snprintf_stub(full,sizeof(full),"%s%s",path,name);
        else                          snprintf_stub(full,sizeof(full),"%s/%s",path,name);

        vfs_stat_t st; memset(&st,0,sizeof(st));
        vfs_stat2(full,&st);

        if (long_fmt) {
            char modestr[11]; fmt_mode(st.mode,st.type,modestr);
            char tstr[24];    fmt_uptime_ms(st.mtime,tstr);

            // Colour by type
            uint32_t nc = (st.type==VFS_DIR)?0x5599FF:
                          (st.type==VFS_CHARDEV||st.type==VFS_BLOCKDEV)?0xFF9922:
                          0xDDDDDD;
            fb_set_color(0x778899,FB_COLOR_BLACK);
            kprintf("  %s %2u %6llu  ",modestr,(unsigned)st.nlinks,(unsigned long long)st.size);
            fb_set_color(0x778899,FB_COLOR_BLACK);
            kprintf("%s  ",tstr);
            fb_set_color(nc,FB_COLOR_BLACK);
            kprintf("%s",name);
            if (st.type==VFS_DIR) kprintf("/");
            kprintf("\n");
        } else {
            // Compact colourised output
            uint32_t nc=(st.type==VFS_DIR)?0x5599FF:
                        (st.type==VFS_CHARDEV)?0xFF9922:
                        (st.type==VFS_BLOCKDEV)?0xBB88FF:0xDDDDDD;
            fb_set_color(nc,FB_COLOR_BLACK);
            kprintf("  %-22s",name);
            if (st.type==VFS_DIR) { fb_set_color(0x5599FF,FB_COLOR_BLACK); kprintf("/"); }
            if (st.type==VFS_FILE) {
                fb_set_color(0x778899,FB_COLOR_BLACK);
                if (st.size>=1024*1024) kprintf("  %4llu MiB",(unsigned long long)(st.size/(1024*1024)));
                else if (st.size>=1024) kprintf("  %4llu KiB",(unsigned long long)(st.size/1024));
                else                   kprintf("  %4llu B  ",(unsigned long long)st.size);
            }
            kprintf("\n");
        }
    }
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("\n");
    vfs_close(fd);
    return 0;
}

// ── mkdir ─────────────────────────────────────────────────────────────────
static int cmd_mkdir(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: mkdir <path>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    if (vfs_mkdir(path,VFS_MODE_DIR)<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  mkdir: cannot create '%s'\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Created directory: %s\n",path);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── rm ────────────────────────────────────────────────────────────────────
static int cmd_rm(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: rm [-r] <path>\n"); return 1; }
    bool recursive = false;
    const char *target_arg = NULL;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"-r")==0) recursive=true;
        else target_arg=argv[i];
    }
    if (!target_arg) { shell_print("Usage: rm [-r] <path>\n"); return 1; }

    char path[VFS_PATH_MAX];
    resolve_path(target_arg,path,sizeof(path));

    uint64_t sz; vfs_node_type_t t;
    if (vfs_stat(path,&sz,&t)<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  rm: '%s' not found\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }

    if (t==VFS_DIR) {
        if (!recursive) {
            fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
            kprintf("  rm: '%s' is a directory  (use -r)\n",path);
            fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
            return 1;
        }
        if (vfs_rmdir(path)<0) {
            fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
            kprintf("  rm: cannot remove '%s' (directory not empty?)\n",path);
            fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
            return 1;
        }
    } else {
        if (vfs_unlink(path)<0) {
            fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
            kprintf("  rm: cannot remove '%s'\n",path);
            fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
            return 1;
        }
    }
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Removed: %s\n",path);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── mv ────────────────────────────────────────────────────────────────────
static int cmd_mv(int argc, char **argv) {
    if (argc<3) { shell_print("Usage: mv <src> <dst>\n"); return 1; }
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    resolve_path(argv[1],src,sizeof(src));
    resolve_path(argv[2],dst,sizeof(dst));
    if (vfs_rename(src,dst)<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  mv: cannot rename '%s' to '%s'\n",src,dst);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Moved: %s -> %s\n",src,dst);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── cp ────────────────────────────────────────────────────────────────────
static int cmd_cp(int argc, char **argv) {
    if (argc<3) { shell_print("Usage: cp <src> <dst>\n"); return 1; }
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    resolve_path(argv[1],src,sizeof(src));
    resolve_path(argv[2],dst,sizeof(dst));

    int sfd = vfs_open(src,O_RDONLY);
    if (sfd<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  cp: cannot open '%s'\n",src);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    int dfd = vfs_open(dst,O_WRONLY|O_CREAT|O_TRUNC);
    if (dfd<0) {
        vfs_close(sfd);
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  cp: cannot create '%s'\n",dst);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    char buf[512];
    ssize_t n;
    uint64_t total=0;
    while ((n=vfs_read(sfd,buf,sizeof(buf)))>0) {
        vfs_write(dfd,buf,(size_t)n);
        total+=(uint64_t)n;
    }
    vfs_close(sfd); vfs_close(dfd);
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Copied %llu bytes: %s -> %s\n",(unsigned long long)total,src,dst);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── touch ─────────────────────────────────────────────────────────────────
static int cmd_touch(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: touch <file>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    if (vfs_touch(path)<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  touch: cannot touch '%s'\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Touched: %s\n",path);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── chmod ─────────────────────────────────────────────────────────────────
static int cmd_chmod(int argc, char **argv) {
    if (argc<3) { shell_print("Usage: chmod <octal> <path>\n"); return 1; }
    uint16_t mode = parse_octal(argv[1]);
    char path[VFS_PATH_MAX];
    resolve_path(argv[2],path,sizeof(path));
    if (vfs_chmod(path,mode)<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  chmod: '%s' not found\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Changed mode of %s to %o\n",path,(unsigned)mode);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── cat ───────────────────────────────────────────────────────────────────
static int cmd_cat(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: cat <file>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    int fd = vfs_open(path,O_RDONLY);
    if (fd<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  cat: '%s': not found\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    char buf[256]; ssize_t n;
    while ((n=vfs_read(fd,buf,sizeof(buf)-1))>0) {
        buf[n]='\0'; shell_print("%s",buf);
    }
    kprintf("\n"); fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    vfs_close(fd); return 0;
}

// ── write ─────────────────────────────────────────────────────────────────
static int cmd_write(int argc, char **argv) {
    if (argc<3) { shell_print("Usage: write <file> <content>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    int fd = vfs_open(path,O_WRONLY|O_CREAT|O_TRUNC);
    if (fd<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  write: cannot open '%s'\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    for (int i=2;i<argc;i++) {
        vfs_write(fd,argv[i],strlen(argv[i]));
        if (i<argc-1) vfs_write(fd," ",1);
    }
    vfs_write(fd,"\n",1);
    vfs_close(fd);
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Wrote to %s\n",path);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── stat ──────────────────────────────────────────────────────────────────
static int cmd_stat(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: stat <path>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    vfs_stat_t st; memset(&st,0,sizeof(st));
    if (vfs_stat2(path,&st)<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  stat: '%s' not found\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    static const char *tnames[]={"file","directory","symlink","chardev","blockdev"};
    char modestr[11]; fmt_mode(st.mode,st.type,modestr);
    char cstr[24],mstr[24],astr[24];
    fmt_uptime_ms(st.ctime,cstr);
    fmt_uptime_ms(st.mtime,mstr);
    fmt_uptime_ms(st.atime,astr);

    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  stat: %s\n",path);
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);

    #define ROW(lbl,fmt,...) \
        fb_set_color(0xAADDFF,FB_COLOR_BLACK); kprintf("  %-10s",lbl); \
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK); kprintf(fmt,##__VA_ARGS__); kprintf("\n");

    ROW("Inode",   "%u",  (unsigned)st.inode_nr);
    ROW("Type",    "%s",  (st.type<5)?tnames[st.type]:"?");
    ROW("Mode",    "%s  (%04o)", modestr, (unsigned)st.mode);
    ROW("Links",   "%u",  (unsigned)st.nlinks);
    ROW("UID:GID", "%u:%u", (unsigned)st.uid, (unsigned)st.gid);
    ROW("Size",    "%llu bytes", (unsigned long long)st.size);
    ROW("Created", "%s", cstr);
    ROW("Modified","%s", mstr);
    ROW("Accessed","%s", astr);
    #undef ROW
    kprintf("\n");
    return 0;
}

// ── hexdump ───────────────────────────────────────────────────────────────
static int cmd_hexdump(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: hexdump <file>\n"); return 1; }
    char path[VFS_PATH_MAX];
    resolve_path(argv[1],path,sizeof(path));
    int fd = vfs_open(path,O_RDONLY);
    if (fd<0) {
        fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
        kprintf("  hexdump: '%s' not found\n",path);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    uint8_t row[16];
    uint64_t offset=0;
    ssize_t n;
    int lines=0;
    while ((n=vfs_read(fd,row,16))>0 && lines<32) {
        fb_set_color(0x778899,FB_COLOR_BLACK);
        kprintf("  %08llx  ",(unsigned long long)offset);
        fb_set_color(0xAADDFF,FB_COLOR_BLACK);
        for (ssize_t i=0;i<16;i++) {
            if (i<n) kprintf("%02x ",(unsigned)row[i]);
            else     kprintf("   ");
            if (i==7) kprintf(" ");
        }
        kprintf("  ");
        fb_set_color(0x44EE88,FB_COLOR_BLACK);
        for (ssize_t i=0;i<n;i++) {
            char c=(char)row[i];
            kprintf("%c",(c>=0x20&&c<0x7F)?c:'.');
        }
        kprintf("\n");
        offset+=(uint64_t)n;
        lines++;
    }
    if (n>0) {
        fb_set_color(0x778899,FB_COLOR_BLACK);
        kprintf("  ... (truncated at 512 bytes)\n");
    }
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    vfs_close(fd);
    return 0;
}

// ── top (shows ALL tasks) ─────────────────────────────────────────────────
typedef struct { int count; } top_ctx_t;

static void top_cb(const task_t *t, void *ud) {
    top_ctx_t *ctx = (top_ctx_t *)ud;
    ctx->count++;

    static const char *states[]={"RUNNABLE","RUNNING ","SLEEPING","BLOCKED ","ZOMBIE  "};
    static const uint32_t scol[]={0xFFDD88,0x44EE88,0x55AAFF,0xFF9922,0xFF4444};
    uint32_t sc = (t->state<5)?scol[t->state]:0xAAAAAA;
    const char *ss = (t->state<5)?states[t->state]:"?       ";

    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("  %-4u ", (unsigned)t->pid);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("%-18s ", t->name);
    fb_set_color(sc,FB_COLOR_BLACK);
    kprintf("%s  ", ss);
    fb_set_color(0xAADDFF,FB_COLOR_BLACK);
    if (t->last_cpu != 0xFF) kprintf("CPU%-2u ", (unsigned)t->last_cpu);
    else                     kprintf("  -   ");
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("%llu\n", (unsigned long long)t->ticks_total);
}

static int cmd_top(int argc, char **argv) {
    (void)argc;(void)argv;
    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  All Tasks  (uptime: %llu ms)\n",
            (unsigned long long)sched_uptime_ms());
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    fb_set_color(0xAADDFF,FB_COLOR_BLACK);
    kprintf("  %-4s %-18s %-8s  %-5s %s\n","PID","NAME","STATE","CPU","TICKS");
    fb_set_color(0x335577,FB_COLOR_BLACK);
    kprintf("  %-4s %-18s %-8s  %-5s %s\n","---","----","-----","---","-----");
    top_ctx_t ctx={0};
    sched_foreach_task(top_cb,&ctx);
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("\n  Total: %d task(s)  |  live: %u\n\n",
            ctx.count, (unsigned)sched_task_count());
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// tasks (legacy alias, now powered by foreach)
static int cmd_tasks(int argc, char **argv) { return cmd_top(argc,argv); }

// ── free (visual memory bar) ──────────────────────────────────────────────
static int cmd_free(int argc, char **argv) {
    (void)argc;(void)argv;
    uint64_t total_pages=0, free_pages=0;
    pmm_get_stats(&total_pages,&free_pages);
    uint64_t used_pages = total_pages - free_pages;
    uint64_t total_mib  = total_pages*4096/(1024*1024);
    uint64_t used_mib   = used_pages *4096/(1024*1024);
    uint64_t free_mib   = free_pages *4096/(1024*1024);
    uint64_t pct        = total_pages ? (used_pages*100)/total_pages : 0;

    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  Memory\n");
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);

    kprintf("  Physical  ");
    draw_bar(used_pages,total_pages,36);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("  %llu/%llu MiB  (%llu%%)\n",
            (unsigned long long)used_mib,
            (unsigned long long)total_mib,
            (unsigned long long)pct);

    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("            Used: %llu MiB   Free: %llu MiB\n\n",
            (unsigned long long)used_mib, (unsigned long long)free_mib);

    fb_set_color(0xAADDFF,FB_COLOR_BLACK);
    kprintf("  Heap slab caches:\n");
    heap_stats();
    kprintf("\n");
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── mem (alias for free + pmm dump) ──────────────────────────────────────
static int cmd_mem(int argc, char **argv) { return cmd_free(argc,argv); }

// ── uptime ────────────────────────────────────────────────────────────────
static int cmd_uptime(int argc, char **argv) {
    (void)argc;(void)argv;
    uint64_t ms=sched_uptime_ms();
    uint64_t s=ms/1000, m=s/60, h=m/60;
    fb_set_color(0x33DDCC,FB_COLOR_BLACK);
    kprintf("  %lluh %llum %llus",
            (unsigned long long)h, (unsigned long long)(m%60),
            (unsigned long long)(s%60));
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("  (%llu ms)\n\n",(unsigned long long)ms);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── cpuinfo ───────────────────────────────────────────────────────────────
static int cmd_cpuinfo(int argc, char **argv) {
    (void)argc;(void)argv;
    char vendor[13], brand[49];
    cpu_vendor(vendor); cpu_brand(brand);
    struct limine_smp_response *smp = limine_smp();
    uint32_t ncpus = smp?(uint32_t)smp->cpu_count:1;

    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  CPU Information\n");
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    #define CROW(l,v,c) fb_set_color(0xAADDFF,FB_COLOR_BLACK);kprintf("  %-10s",l);\
                         fb_set_color(c,FB_COLOR_BLACK);kprintf("%s\n",v);
    char ncbuf[8]; ncbuf[0]='0'+(char)(ncpus%10); ncbuf[1]='\0';
    CROW("Vendor",  vendor,  FB_COLOR_WHITE);
    CROW("Brand",   brand,   FB_COLOR_WHITE);
    CROW("CPUs",    ncbuf,   0x44EE88);
    CROW("x2APIC",  cpu_has_x2apic()?"yes":"no", cpu_has_x2apic()?0x44EE88:0xFF9922);
    CROW("NX bit",  cpu_has_nx()?"yes":"no",      cpu_has_nx()?0x44EE88:0xFF4444);
    CROW("TSC",     cpu_has_tsc()?"yes":"no",      cpu_has_tsc()?0x44EE88:0xFF9922);
    CROW("InvTSC",  cpu_has_invariant_tsc()?"yes":"no",
                    cpu_has_invariant_tsc()?0x44EE88:0xFF9922);
    #undef CROW
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("\n");
    return 0;
}

// ── sleep ─────────────────────────────────────────────────────────────────
static int cmd_sleep(int argc, char **argv) {
    if (argc<2) { shell_print("Usage: sleep <ms>\n"); return 1; }
    uint64_t ms=0;
    for (const char *p=argv[1];*p>='0'&&*p<='9';p++) ms=ms*10+(uint64_t)(*p-'0');
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("  Sleeping %llu ms...\n",(unsigned long long)ms);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    uint64_t t0 = sched_uptime_ms();
    sched_sleep_ms(ms);
    uint64_t elapsed = sched_uptime_ms() - t0;
    fb_set_color(0x44EE88,FB_COLOR_BLACK);
    kprintf("  Awake. (slept %llu ms)\n",(unsigned long long)elapsed);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── history ───────────────────────────────────────────────────────────────
static int cmd_history(int argc, char **argv) {
    (void)argc;(void)argv;
    fb_set_color(0x00D4FF,FB_COLOR_BLACK); kprintf("\n  History\n");
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    int start = hist_count>SHELL_HIST_MAX?hist_count-SHELL_HIST_MAX:0;
    for (int i=start;i<hist_count;i++) {
        fb_set_color(0x778899,FB_COLOR_BLACK); kprintf("  %3d  ",i+1);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        kprintf("%s\n",history[i%SHELL_HIST_MAX]);
    }
    kprintf("\n"); fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── disk ──────────────────────────────────────────────────────────────────
static int cmd_disk(int argc, char **argv) {
    (void)argc;(void)argv;
    virtio_blk_info_t info; virtio_blk_info(&info);
    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n  VirtIO Block Device\n");
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    if (!info.capacity) {
        fb_set_color(0xFF9922,FB_COLOR_BLACK); kprintf("  No disk detected.\n\n");
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK); return 1;
    }
    uint64_t mib=info.capacity*512/(1024*1024);
    fb_set_color(0xAADDFF,FB_COLOR_BLACK); kprintf("  Capacity  ");
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    kprintf("%llu sectors  (%llu MiB)\n",(unsigned long long)info.capacity,(unsigned long long)mib);
    kprintf("  Usage     "); draw_bar(0,mib,28);
    kprintf("  0%%\n");
    fb_set_color(0xAADDFF,FB_COLOR_BLACK); kprintf("  ReadOnly  ");
    fb_set_color(info.read_only?0xFF9922:0x44EE88,FB_COLOR_BLACK);
    kprintf("%s\n\n",info.read_only?"yes":"no");
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── kv (persistent key-value store) ──────────────────────────────────────
static int kv_list_cb(const char *k, const char *v, void *ud) {
    (void)ud;
    fb_set_color(0x33DDCC,FB_COLOR_BLACK); kprintf("  %-28s",k);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK); kprintf("%s\n",v);
    return 1; // continue
}

static int cmd_kv(int argc, char **argv) {
    if (!kv_ready()) {
        fb_set_color(0xFF9922,FB_COLOR_BLACK);
        kprintf("  [KV] No persistent storage available.\n");
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 1;
    }
    if (argc<2 || strcmp(argv[1],"list")==0) {
        fb_set_color(0x00D4FF,FB_COLOR_BLACK); kprintf("\n  Persistent Store\n");
        fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
        kv_list(kv_list_cb,NULL);
        kprintf("\n"); fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 0;
    }
    if (strcmp(argv[1],"set")==0) {
        if (argc<4) { shell_print("Usage: kv set <key> <value>\n"); return 1; }
        if (kv_set(argv[2],argv[3])<0) {
            fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
            kprintf("  kv: set failed (store full or key too long)\n");
            fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
            return 1;
        }
        fb_set_color(0x44EE88,FB_COLOR_BLACK);
        kprintf("  Saved: %s = %s\n",argv[2],argv[3]);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 0;
    }
    if (strcmp(argv[1],"get")==0) {
        if (argc<3) { shell_print("Usage: kv get <key>\n"); return 1; }
        char val[KVSTORE_VAL_MAX];
        if (kv_get(argv[2],val,sizeof(val))<0) {
            fb_set_color(0xFF9922,FB_COLOR_BLACK);
            kprintf("  kv: key '%s' not found\n",argv[2]);
            fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
            return 1;
        }
        fb_set_color(0x33DDCC,FB_COLOR_BLACK); kprintf("  %s ",argv[2]);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK); kprintf("= %s\n",val);
        return 0;
    }
    if (strcmp(argv[1],"del")==0) {
        if (argc<3) { shell_print("Usage: kv del <key>\n"); return 1; }
        if (kv_del(argv[2])<0) {
            fb_set_color(0xFF9922,FB_COLOR_BLACK);
            kprintf("  kv: key '%s' not found\n",argv[2]);
            fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
            return 1;
        }
        fb_set_color(0x44EE88,FB_COLOR_BLACK);
        kprintf("  Deleted: %s\n",argv[2]);
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 0;
    }
    shell_print("  kv: unknown subcommand '%s'\n",argv[1]);
    shell_print("  Usage: kv [list | set <k> <v> | get <k> | del <k>]\n");
    return 1;
}

// ── reboot ────────────────────────────────────────────────────────────────
static int cmd_reboot(int argc, char **argv) {
    (void)argc;(void)argv;
    fb_set_color(0xFF9922,FB_COLOR_BLACK); kprintf("  Rebooting...\n");
    __asm__ volatile ("lidt (%%rsp)\nint3\n":::"memory");
    for(;;);
}

// ── version ───────────────────────────────────────────────────────────────
static int cmd_version(int argc, char **argv) {
    (void)argc;(void)argv;
    fb_set_color(0x00D4FF,FB_COLOR_BLACK);
    kprintf("\n"
        "   ____                    _       __  ____   _____ \n"
        "  / __ \\____  ____ _   __(_)___ _/ / / __ \\ / ___/ \n"
        " / / / / __ \\/ __ \\ | / / / __ `/ / / / / / \\__ \\  \n"
        "/ /_/ / / / / / / / |/ / / /_/ / / / /_/ / ___/ /  \n"
        "\\____/_/ /_/_/ /_/|___/_/\\__,_/_/ /_____/ /____/   \n");
    fb_set_color(0xAADDFF,FB_COLOR_BLACK);
    kprintf("  Quanta OS  v%s  (%s)\n", QUANTA_VERSION, QUANTA_ARCH);
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("  x86-64 | x2APIC | SMP | VirtIO | QuantaFS | KV-Store | QAI\n\n");
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 0;
}

// ── ai ────────────────────────────────────────────────────────────────────
static int cmd_ai(int argc, char **argv) {
    if (argc<2) {
        fb_set_color(0xBB88FF,FB_COLOR_BLACK);
        kprintf("  [QAI] Usage: ai <question>\n");
        fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
        return 0;
    }
    char question[SHELL_LINE_MAX]={0};
    for (int i=1;i<argc;i++) {
        if (i>1) strcat_safe(question," ",sizeof(question));
        strcat_safe(question,argv[i],sizeof(question));
    }
    qai_answer(question); return 0;
}

// ── Helpers ───────────────────────────────────────────────────────────────
static void snprintf_stub(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt);
    size_t pos=0;
    while (*fmt && pos<n-1) {
        if (fmt[0]=='%'&&fmt[1]=='s') {
            const char *s=va_arg(ap,const char*);
            while (*s&&pos<n-1) buf[pos++]=*s++;
            fmt+=2;
        } else buf[pos++]=*fmt++;
    }
    buf[pos]='\0'; va_end(ap);
}
static void strcat_safe(char *dst, const char *src, size_t max) {
    size_t l=strlen(dst), r=max-l-1, i=0;
    while (i<r&&src[i]) { dst[l+i]=src[i]; i++; }
    dst[l+i]='\0';
}

// ── Register builtins ─────────────────────────────────────────────────────
static void register_builtins(void) {
    // System
    shell_register_cat("version", "Show Quanta banner",               "System", cmd_version);
    shell_register_cat("cpuinfo", "CPU features and topology",        "System", cmd_cpuinfo);
    shell_register_cat("mem",     "Memory usage (visual bar)",        "System", cmd_mem);
    shell_register_cat("free",    "Same as mem",                      "System", cmd_free);
    shell_register_cat("uptime",  "System uptime",                    "System", cmd_uptime);
    shell_register_cat("top",     "All tasks with CPU/tick stats",    "System", cmd_top);
    shell_register_cat("tasks",   "Alias for top",                    "System", cmd_tasks);
    shell_register_cat("disk",    "VirtIO block device info",         "System", cmd_disk);
    shell_register_cat("reboot",  "Reboot the system",                "System", cmd_reboot);
    // Files
    shell_register_cat("ls",      "List dir  (-l for long format)",   "Files",  cmd_ls);
    shell_register_cat("cat",     "Print file contents",              "Files",  cmd_cat);
    shell_register_cat("write",   "Write text to a file",             "Files",  cmd_write);
    shell_register_cat("stat",    "Full inode metadata",              "Files",  cmd_stat);
    shell_register_cat("mkdir",   "Create directory",                 "Files",  cmd_mkdir);
    shell_register_cat("rm",      "Remove file or dir (-r)",          "Files",  cmd_rm);
    shell_register_cat("mv",      "Rename / move",                    "Files",  cmd_mv);
    shell_register_cat("cp",      "Copy a file",                      "Files",  cmd_cp);
    shell_register_cat("touch",   "Create or update mtime",           "Files",  cmd_touch);
    shell_register_cat("chmod",   "Change permissions (octal)",       "Files",  cmd_chmod);
    shell_register_cat("hexdump", "Hex+ASCII dump (first 512 B)",     "Files",  cmd_hexdump);
    shell_register_cat("cd",      "Change directory",                 "Files",  cmd_cd);
    shell_register_cat("pwd",     "Print working directory",          "Files",  cmd_pwd);
    // Shell
    shell_register_cat("help",    "List all commands by category",    "Shell",  cmd_help);
    shell_register_cat("clear",   "Clear the screen",                 "Shell",  cmd_clear);
    shell_register_cat("echo",    "Print arguments",                  "Shell",  cmd_echo);
    shell_register_cat("sleep",   "Sleep N milliseconds",             "Shell",  cmd_sleep);
    shell_register_cat("history", "Show command history",             "Shell",  cmd_history);
    // Persistence
    shell_register_cat("kv",      "Persistent store: set/get/del/list","Shell", cmd_kv);
    // AI
    shell_register_cat("ai",      "Ask the QAI assistant",            "AI",     cmd_ai);
}

// ── Execute ───────────────────────────────────────────────────────────────
static int execute(char *line) {
    int len=(int)strlen(line);
    while (len>0&&line[len-1]==' ') line[--len]='\0';
    if (!len) return 0;
    hist_push(line);
    char *argv[SHELL_ARGS_MAX]; int argc=parse_args(line,argv,SHELL_ARGS_MAX);
    if (!argc) return 0;
    for (int i=0;i<cmd_count;i++)
        if (strcmp(cmd_table[i].name,argv[0])==0) return cmd_table[i].fn(argc,argv);
    fb_set_color(FB_COLOR_RED,FB_COLOR_BLACK);
    kprintf("  Unknown: %s",argv[0]);
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("  (type ");
    fb_set_color(0x33DDCC,FB_COLOR_BLACK); kprintf("help");
    fb_set_color(0x778899,FB_COLOR_BLACK); kprintf(")\n");
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    return 1;
}

// ── Prompt ────────────────────────────────────────────────────────────────
static void print_prompt(void) {
    fb_statusbar_refresh();
    fb_set_color(0x335577,FB_COLOR_BLACK);
    kprintf("  -- %s\n",shell_cwd);
    fb_set_color(0x33DDCC,FB_COLOR_BLACK);  kprintf("quanta");
    fb_set_color(0x778899,FB_COLOR_BLACK);  kprintf("@");
    fb_set_color(0xAADDFF,FB_COLOR_BLACK);  kprintf("kernel");
    fb_set_color(0x778899,FB_COLOR_BLACK);  kprintf(":");
    fb_set_color(0xFFDD88,FB_COLOR_BLACK);  kprintf("%s",shell_cwd);
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK); kprintf("$ ");
}

// ── shell_run ─────────────────────────────────────────────────────────────
void shell_run(void *arg) {
    (void)arg;
    register_builtins();

    // Welcome
    cmd_version(0,NULL);
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    fb_set_color(0x778899,FB_COLOR_BLACK);
    kprintf("  Tab=complete  Up/Down=history  ls -l for long listing\n");
    kprintf("  kv set/get/del/list for persistent storage\n");
    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK);
    fb_draw_hline('-',0x335577,FB_COLOR_BLACK);
    kprintf("\n");

    for (;;) {
        print_prompt();
        input_buf[0]='\0'; input_len=0; cursor=0; hist_pos=hist_count;
        int done=0;
        while (!done) {
            char c=kbd_getchar();
            switch (c) {
            case '\n': case '\r': kprintf("\n"); done=1; break;
            case '\b': case 127:  line_backspace(); break;
            case '\033': {
                char s1=kbd_getchar_noblock(), s2=kbd_getchar_noblock();
                if (s1=='[') {
                    if (s2=='A'&&hist_pos>0) {
                        hist_pos--;
                        line_clear_display();
                        strncpy(input_buf,history[hist_pos%SHELL_HIST_MAX],SHELL_LINE_MAX-1);
                        input_buf[SHELL_LINE_MAX-1]='\0';
                        input_len=cursor=(int)strlen(input_buf);
                        line_redraw();
                    } else if (s2=='B'&&hist_pos<hist_count) {
                        hist_pos++;
                        line_clear_display();
                        if (hist_pos==hist_count) { input_buf[0]='\0'; input_len=cursor=0; }
                        else {
                            strncpy(input_buf,history[hist_pos%SHELL_HIST_MAX],SHELL_LINE_MAX-1);
                            input_buf[SHELL_LINE_MAX-1]='\0';
                            input_len=cursor=(int)strlen(input_buf);
                        }
                        line_redraw();
                    } else if (s2=='C'&&cursor<input_len) { cursor++; kprintf("\033[C"); }
                      else if (s2=='D'&&cursor>0)          { cursor--; kprintf("\033[D"); }
                }
                break;
            }
            case '\t': {
                if (!input_len) break;
                int matches=0; const char *last=NULL;
                for (int i=0;i<cmd_count;i++)
                    if (strncmp(cmd_table[i].name,input_buf,input_len)==0) { matches++; last=cmd_table[i].name; }
                if (matches==1&&last) {
                    line_clear_display();
                    strncpy(input_buf,last,SHELL_LINE_MAX-1); input_buf[SHELL_LINE_MAX-1]='\0';
                    input_len=cursor=(int)strlen(input_buf); line_redraw();
                } else if (matches>1) {
                    kprintf("\n");
                    for (int i=0;i<cmd_count;i++)
                        if (strncmp(cmd_table[i].name,input_buf,input_len)==0) {
                            fb_set_color(0x33DDCC,FB_COLOR_BLACK);
                            kprintf("  %s",cmd_table[i].name);
                        }
                    fb_set_color(FB_COLOR_WHITE,FB_COLOR_BLACK); kprintf("\n");
                    print_prompt(); kprintf("%s",input_buf);
                }
                break;
            }
            default:
                if (c>=0x20&&c<0x7F) line_insert(c);
                break;
            }
        }
        execute(input_buf);
    }
}
