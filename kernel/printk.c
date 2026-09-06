#include "printk.h"
#include "klog.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../lib/printf.h"
#include <stdarg.h>
#include <stddef.h>

void vprintk(const char *fmt, va_list args) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    serial_puts(buf);
    vga_puts(buf);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    klog_write(buf, (size_t)n);
}

void printk(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
}
