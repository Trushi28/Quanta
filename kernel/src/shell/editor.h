#pragma once
// ============================================================
//  shell/editor.h — Quanta built-in text editor
//
//  A nano-style single-file editor that runs entirely inside
//  the kernel shell.  Uses the existing framebuffer terminal
//  and VFS for reading/writing.
//
//  Keybindings:
//    Arrows / Home / End / PgUp / PgDn — cursor movement
//    Printable chars                   — insert at cursor
//    Backspace / Delete                — delete character
//    Enter                             — split line
//    ^S  (Ctrl-S)                      — save
//    ^Q  (Ctrl-Q)                      — quit (prompts if unsaved)
//    ^K  (Ctrl-K)                      — cut current line
//    ^U  (Ctrl-U)                      — paste cut line
//    ^G  (Ctrl-G)                      — go to line number
//    ^F  (Ctrl-F)                      — find (substring search)
//    ^H  (Ctrl-H)                      — show help overlay
// ============================================================

// Open (or create) a file in the editor.
// Blocks until the user exits.
// Returns  0 on clean exit.
// Returns -1 if the file could not be opened or created.
int ed_open(const char *path);
