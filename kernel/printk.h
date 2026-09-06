#pragma once
#include <stdarg.h>

void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(const char *fmt, va_list args);
