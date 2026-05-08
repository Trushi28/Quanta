#pragma once
#include <stdarg.h>

int   kvprintf(const char *fmt, va_list args);
__attribute__((format(printf,1,2))) int kprintf(const char *fmt, ...);
__attribute__((noreturn,format(printf,1,2))) void kpanic(const char *fmt, ...);
