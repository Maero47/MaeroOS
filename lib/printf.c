#include "printf.h"
#include <stdint.h>

/*
 * vsnprintf — kernel-mode formatted string writer.
 *
 * Supported specifiers: %d/%i, %u, %x/%X, %o, %c, %s, %p, %%
 * Flags: zero-padding (0), width specifier, left-align (-)
 * No floating point.
 */

static void write_char(char *buf, size_t *pos, size_t n, char c) {
    if (*pos < n - 1)
        buf[*pos] = c;
    (*pos)++;
}

static void write_str(char *buf, size_t *pos, size_t n, const char *s,
                      int width, int left_align) {
    int len = 0;
    for (const char *p = s; *p; p++) len++;

    if (!left_align) {
        for (int i = len; i < width; i++)
            write_char(buf, pos, n, ' ');
    }
    while (*s) write_char(buf, pos, n, *s++);
    if (left_align) {
        for (int i = len; i < width; i++)
            write_char(buf, pos, n, ' ');
    }
}

static void write_uint(char *buf, size_t *pos, size_t n, uint32_t val,
                       int base, int upper, int width, int zero_pad,
                       int left_align) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;

    char tmp[32];
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        uint32_t v = val;
        while (v) {
            tmp[len++] = digits[v % (uint32_t)base];
            v /= (uint32_t)base;
        }
    }

    char pad = zero_pad ? '0' : ' ';
    if (!left_align) {
        for (int i = len; i < width; i++)
            write_char(buf, pos, n, pad);
    }
    for (int i = len - 1; i >= 0; i--)
        write_char(buf, pos, n, tmp[i]);
    if (left_align) {
        for (int i = len; i < width; i++)
            write_char(buf, pos, n, ' ');
    }
}

static void write_int(char *buf, size_t *pos, size_t n, int32_t val,
                      int width, int zero_pad, int left_align) {
    if (val < 0) {
        write_char(buf, pos, n, '-');
        write_uint(buf, pos, n, (uint32_t)-val, 10, 0, width > 1 ? width-1 : 0,
                   zero_pad, left_align);
    } else {
        write_uint(buf, pos, n, (uint32_t)val, 10, 0, width, zero_pad, left_align);
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list args) {
    if (!buf || n == 0) return 0;

    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            write_char(buf, &pos, n, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags */
        int left_align = 0, zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad   = 1;
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* Dispatch on specifier */
        char spec = *fmt++;
        switch (spec) {
        case 'd': case 'i':
            write_int(buf, &pos, n, va_arg(args, int32_t), width, zero_pad, left_align);
            break;
        case 'u':
            write_uint(buf, &pos, n, va_arg(args, uint32_t), 10, 0,
                       width, zero_pad, left_align);
            break;
        case 'x':
            write_uint(buf, &pos, n, va_arg(args, uint32_t), 16, 0,
                       width, zero_pad, left_align);
            break;
        case 'X':
            write_uint(buf, &pos, n, va_arg(args, uint32_t), 16, 1,
                       width, zero_pad, left_align);
            break;
        case 'o':
            write_uint(buf, &pos, n, va_arg(args, uint32_t), 8, 0,
                       width, zero_pad, left_align);
            break;
        case 'p': {
            /* Pointer: 0x + 8 hex digits */
            write_char(buf, &pos, n, '0');
            write_char(buf, &pos, n, 'x');
            write_uint(buf, &pos, n, (uint32_t)(uintptr_t)va_arg(args, void *),
                       16, 0, 8, 1, 0);
            break;
        }
        case 'c':
            write_char(buf, &pos, n, (char)va_arg(args, int));
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            write_str(buf, &pos, n, s, width, left_align);
            break;
        }
        case '%':
            write_char(buf, &pos, n, '%');
            break;
        default:
            write_char(buf, &pos, n, '%');
            write_char(buf, &pos, n, spec);
            break;
        }
    }

    /* Always null-terminate */
    buf[pos < n ? pos : n - 1] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, n, fmt, args);
    va_end(args);
    return ret;
}
