#pragma once
#include <stdarg.h>

void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(const char *fmt, va_list args);

/* Kernel log ring only (dmesg, /proc/kmsg), not the console: for status a
 * background thread reports at an arbitrary moment (a DHCP lease, a NIC
 * reset), which would otherwise land in the middle of whatever a user or a
 * test harness is reading on the serial/VGA console. */
void printk_klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
