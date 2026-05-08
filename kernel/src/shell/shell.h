#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  shell/shell.h — Quanta AI Interactive Shell (QAI)
//
//  A kernel-mode interactive shell that runs as a scheduled task.
//  Features:
//    - Built-in commands: help, ls, cat, echo, clear, mem, cpuinfo,
//                         uptime, tasks, disk, reboot, history
//    - Command history (arrow-key navigation)
//    - Tab completion
//    - Pipe-like output redirection to /tmp files
//    - "ai" command: sends a prompt to the Quanta AI assistant
//      (rule-based, no network needed — Quanta's built-in knowledge base)
// ---------------------------------------------------------------------------

// Maximum input line length
#define SHELL_LINE_MAX   512
#define SHELL_HIST_MAX   64
#define SHELL_ARGS_MAX   32
#define SHELL_PROMPT     "\033[96mquanta\033[0m\033[90m@kernel\033[0m:\033[93m~\033[0m$ "

// ANSI escape helpers for the shell
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[91m"
#define ANSI_GREEN   "\033[92m"
#define ANSI_YELLOW  "\033[93m"
#define ANSI_BLUE    "\033[94m"
#define ANSI_MAGENTA "\033[95m"
#define ANSI_CYAN    "\033[96m"
#define ANSI_WHITE   "\033[97m"
#define ANSI_GRAY    "\033[90m"

// Start the interactive shell task (never returns).
void shell_run(void *arg);

// Register a built-in command handler.
typedef int (*shell_cmd_fn)(int argc, char **argv);
void shell_register(const char *name, const char *help, shell_cmd_fn fn);

// Print to the shell output (uses kprintf internally).
void shell_print(const char *fmt, ...) __attribute__((format(printf,1,2)));
