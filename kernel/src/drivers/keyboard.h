#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  drivers/keyboard.h — PS/2 Keyboard driver (IRQ1)
// ---------------------------------------------------------------------------
void keyboard_init(void);

// Blocking: wait for a character and return it (ASCII).
char kbd_getchar(void);

// Non-blocking: return next char or 0 if none available.
char kbd_getchar_noblock(void);
