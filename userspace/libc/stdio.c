#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include <stdarg.h>
#include <stdint.h>
#include "../include/syscall.h"

/* ── FILE struct ─────────────────────────────────────────────────────────────── */

#define FILE_BUFSZ  512
#define FILE_RDONLY 1
#define FILE_WRONLY 2
#define FILE_RDWR   3
#define FILE_APPEND 4

struct _FILE {
    int   fd;
    int   mode;       /* FILE_RDONLY etc. */
    int   error;
    int   eof;
    /* write buffer */
    char  wbuf[FILE_BUFSZ];
    int   wpos;
    /* read buffer */
    char  rbuf[FILE_BUFSZ];
    int   rpos;
    int   rlen;
    /* for ungetc */
    int   unget;      /* -1 = none */
};

/* Static slots for the three standard streams */
static struct _FILE _stdin_s  = { .fd = 0, .mode = FILE_RDONLY, .unget = -1 };
static struct _FILE _stdout_s = { .fd = 1, .mode = FILE_WRONLY, .unget = -1 };
static struct _FILE _stderr_s = { .fd = 2, .mode = FILE_WRONLY, .unget = -1 };

FILE *stdin  = &_stdin_s;
FILE *stdout = &_stdout_s;
FILE *stderr = &_stderr_s;

/* ── Internal flush ──────────────────────────────────────────────────────────── */

static int _fflush_unlocked(FILE *f) {
    if (f->wpos > 0) {
        write(f->fd, f->wbuf, f->wpos);
        f->wpos = 0;
    }
    return 0;
}

/* ── fopen / fclose ──────────────────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode) {
    int flags = 0, fmode = FILE_RDONLY;
    if (mode[0] == 'r') {
        flags  = 0;          /* O_RDONLY */
        fmode  = FILE_RDONLY;
        if (mode[1] == '+') { flags = 2; fmode = FILE_RDWR; }
    } else if (mode[0] == 'w') {
        flags  = 1 | 0x040 | 0x200; /* O_WRONLY|O_CREAT|O_TRUNC */
        fmode  = FILE_WRONLY;
        if (mode[1] == '+') { flags = 2 | 0x040 | 0x200; fmode = FILE_RDWR; }
    } else if (mode[0] == 'a') {
        flags  = 1 | 0x040 | 0x400; /* O_WRONLY|O_CREAT|O_APPEND */
        fmode  = FILE_APPEND;
        if (mode[1] == '+') { flags = 2 | 0x040 | 0x400; fmode = FILE_RDWR; }
    }
    int fd = open(path, flags);
    if (fd < 0) return (FILE *)0;

    FILE *f = malloc(sizeof(struct _FILE));
    if (!f) { close(fd); return (FILE *)0; }
    memset(f, 0, sizeof(struct _FILE));
    f->fd    = fd;
    f->mode  = fmode;
    f->unget = -1;
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    int fmode = FILE_RDONLY;
    if (!mode || fd < 0) return (FILE *)0;
    if (mode[0] == 'w') fmode = FILE_WRONLY;
    else if (mode[0] == 'a') fmode = FILE_APPEND;
    if (mode[1] == '+') fmode = FILE_RDWR;

    FILE *f = malloc(sizeof(struct _FILE));
    if (!f) return (FILE *)0;
    memset(f, 0, sizeof(struct _FILE));
    f->fd = fd;
    f->mode = fmode;
    f->unget = -1;
    return f;
}

int fclose(FILE *f) {
    if (!f) return -1;
    _fflush_unlocked(f);
    int r = close(f->fd);
    if (f != stdin && f != stdout && f != stderr)
        free(f);
    return r;
}

int fflush(FILE *f) {
    if (!f) return 0;
    return _fflush_unlocked(f);
}

/* ── fread / fwrite ──────────────────────────────────────────────────────────── */

size_t fwrite(const void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f || f->error) return 0;
    size_t total = sz * n;
    if (!total) return 0;
    const char *p = ptr;
    /* Line-buffered: buffer until newline or full, then flush */
    for (size_t i = 0; i < total; i++) {
        f->wbuf[f->wpos++] = p[i];
        if (f->wpos == FILE_BUFSZ || p[i] == '\n') {
            _fflush_unlocked(f);
        }
    }
    return n;
}

size_t fread(void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f || f->error || f->eof) return 0;
    size_t total = sz * n;
    if (!total) return 0;
    char *p = ptr;
    size_t done = 0;

    /* Drain ungetc byte first */
    if (f->unget >= 0 && done < total) {
        p[done++] = (char)f->unget;
        f->unget = -1;
    }

    /* Drain read buffer */
    while (done < total && f->rpos < f->rlen) {
        p[done++] = f->rbuf[f->rpos++];
    }

    /* Direct read for remainder */
    while (done < total) {
        int r = read(f->fd, p + done, (int)(total - done));
        if (r <= 0) { if (r == 0) f->eof = 1; else f->error = 1; break; }
        done += (size_t)r;
    }
    return sz ? done / sz : 0;
}

/* ── fputc / fgetc ───────────────────────────────────────────────────────────── */

int fputc(int c, FILE *f) {
    if (!f || f->error) return EOF;
    char ch = (char)c;
    f->wbuf[f->wpos++] = ch;
    if (f->wpos == FILE_BUFSZ || ch == '\n')
        _fflush_unlocked(f);
    return (unsigned char)c;
}

int fgetc(FILE *f) {
    if (!f || f->error || f->eof) return EOF;
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->rpos >= f->rlen) {
        int r = read(f->fd, f->rbuf, FILE_BUFSZ);
        if (r <= 0) { if (r == 0) f->eof = 1; else f->error = 1; return EOF; }
        f->rpos = 0; f->rlen = r;
    }
    return (unsigned char)f->rbuf[f->rpos++];
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    f->unget = (unsigned char)c;
    f->eof   = 0;
    return c;
}

/* ── fputs / fgets ───────────────────────────────────────────────────────────── */

int fputs(const char *s, FILE *f) {
    while (*s) if (fputc((unsigned char)*s++, f) == EOF) return EOF;
    return 0;
}

char *fgets(char *buf, int size, FILE *f) {
    if (!buf || size <= 0 || !f) return (char *)0;
    int i = 0;
    for (; i < size - 1; i++) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return (char *)0; break; }
        buf[i] = (char)c;
        if (c == '\n') { i++; break; }
    }
    buf[i] = '\0';
    return buf;
}

/* ── fseek / ftell / rewind ──────────────────────────────────────────────────── */

int fseek(FILE *f, long offset, int whence) {
    if (!f) return -1;
    _fflush_unlocked(f);
    f->rpos = f->rlen = 0;
    f->unget = -1;
    f->eof   = 0;
    return lseek(f->fd, (int)offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    return (long)lseek(f->fd, 0, 1 /* SEEK_CUR */) - (f->rlen - f->rpos);
}

void rewind(FILE *f) { fseek(f, 0, 0 /* SEEK_SET */); }

/* ── feof / ferror / clearerr ────────────────────────────────────────────────── */

int feof(FILE *f)   { return f ? f->eof   : 1; }
int ferror(FILE *f) { return f ? f->error : 1; }
void clearerr(FILE *f) { if (f) { f->eof = 0; f->error = 0; } }

int fileno(FILE *f) { return f ? f->fd : -1; }

/* ── vsnprintf — the core formatting engine ──────────────────────────────────── */

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;

#define OUT(c) do { if (pos < cap - 1) buf[pos] = (c); pos++; } while(0)

    while (*fmt) {
        if (*fmt != '%') { OUT(*fmt++); continue; }
        fmt++; /* skip % */

        /* Flags */
        int flag_zero = 0, flag_left = 0, flag_plus = 0, flag_space = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '0') flag_zero = 1;
            if (*fmt == '-') flag_left = 1;
            if (*fmt == '+') flag_plus = 1;
            if (*fmt == ' ') flag_space = 1;
            fmt++;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; /* ll */ }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { is_long = 1; fmt++; }

        char spec = *fmt++;
        if (!spec) break;

        if (spec == '%') { OUT('%'); continue; }
        if (spec == 'c') { OUT((char)va_arg(ap, int)); continue; }

        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            int pad = width - len;
            if (!flag_left) while (pad-- > 0) OUT(' ');
            for (int i = 0; i < len; i++) OUT(s[i]);
            if (flag_left)  while (pad-- > 0) OUT(' ');
            continue;
        }

        if (spec == 'n') { *(va_arg(ap, int *)) = (int)pos; continue; }

        /* Numeric conversions */
        char nbuf[32]; int nlen = 0;
        unsigned long uval = 0;
        int is_signed = 0, negative = 0;
        int base = 10;
        int upper = 0;
        char prefix[3] = {0,0,0};

        switch (spec) {
        case 'd': case 'i':
            is_signed = 1;
            { long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
              if (v < 0) { negative = 1; uval = (unsigned long)-v; }
              else uval = (unsigned long)v; }
            break;
        case 'u':
            uval = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            break;
        case 'o': base = 8;
            uval = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            break;
        case 'x': base = 16;
            uval = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            break;
        case 'X': base = 16; upper = 1;
            uval = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            break;
        case 'p': base = 16; is_long = 1; prefix[0]='0'; prefix[1]='x';
            uval = (unsigned long)(uintptr_t)va_arg(ap, void *);
            break;
        default:
            OUT('?'); continue;
        }

        /* Build number string backwards */
        const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        if (!uval) { nbuf[nlen++] = '0'; }
        else { while (uval) { nbuf[nlen++] = digits[uval % base]; uval /= base; } }

        /* Integer precision = minimum digit count (zero-pad): %.3d → 033 */
        if (prec > 0) {
            while (nlen < prec && nlen < (int)sizeof(nbuf) - 1)
                nbuf[nlen++] = '0';
        }

        /* Build sign/prefix */
        char sign = 0;
        if (is_signed) {
            if (negative) sign = '-';
            else if (flag_plus) sign = '+';
            else if (flag_space) sign = ' ';
        }

        int plen = prefix[0] ? (prefix[1] ? 2 : 1) : 0;
        int total_len = nlen + (sign ? 1 : 0) + plen;
        int pad = width - total_len;

        if (!flag_left && !flag_zero) while (pad-- > 0) OUT(' ');
        if (sign) OUT(sign);
        for (int i = 0; prefix[i]; i++) OUT(prefix[i]);
        if (!flag_left &&  flag_zero) while (pad-- > 0) OUT('0');
        for (int i = nlen - 1; i >= 0; i--) OUT(nbuf[i]);
        if ( flag_left)               while (pad-- > 0) OUT(' ');
    }
#undef OUT
    if (cap > 0) buf[pos < cap ? pos : cap - 1] = '\0';
    return (int)pos;
}

/* ── printf family ───────────────────────────────────────────────────────────── */

int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(1, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap); return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int out = n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1;
    if (f) fwrite(buf, 1, (size_t)out, f);
    else write(1, buf, out);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    int out = n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1;
    if (f) fwrite(buf, 1, (size_t)out, f);
    else write(1, buf, out);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap); return n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap); return n;
}

int asprintf(char **strp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    *strp = malloc((size_t)(n + 1));
    if (!*strp) return -1;
    memcpy(*strp, tmp, (size_t)(n + 1));
    return n;
}

/* ── putchar / puts ──────────────────────────────────────────────────────────── */

int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout); }
int putc(int c, FILE *f) { return fputc(c, f); }
int getc(FILE *f) { return fgetc(f); }

int getchar(void) {
    char c;
    int r = read(0, &c, 1);
    if (r <= 0) return EOF;
    return (unsigned char)c;
}

/* ── sscanf (basic) ──────────────────────────────────────────────────────────── */

int vsscanf(const char *s, const char *fmt, va_list ap) {
    int n = 0;
    while (*fmt && *s) {
        if (*fmt == '%') {
            fmt++;
            int suppress = 0;
            if (*fmt == '*') { suppress = 1; fmt++; }
            /* width — skip */
            while (*fmt >= '0' && *fmt <= '9') fmt++;
            char spec = *fmt++;
            if (!spec) break;

            /* skip leading whitespace */
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;

            if (spec == 'd' || spec == 'i') {
                long v = 0; int neg = 0;
                if (*s == '-') { neg = 1; s++; }
                else if (*s == '+') s++;
                int got = 0;
                while (*s >= '0' && *s <= '9') { v = v*10+(*s-'0'); s++; got=1; }
                if (!got) break;
                if (!suppress) { *va_arg(ap, int *) = (int)(neg?-v:v); n++; }
            } else if (spec == 'u') {
                unsigned long v = 0; int got = 0;
                while (*s >= '0' && *s <= '9') { v = v*10+(*s-'0'); s++; got=1; }
                if (!got) break;
                if (!suppress) { *va_arg(ap, unsigned int *) = (unsigned int)v; n++; }
            } else if (spec == 'x' || spec == 'X') {
                unsigned long v = 0; int got = 0;
                if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) s+=2;
                while ((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')||(*s>='A'&&*s<='F')) {
                    int d = (*s>='a') ? *s-'a'+10 : (*s>='A') ? *s-'A'+10 : *s-'0';
                    v = v*16+d; s++; got=1;
                }
                if (!got) break;
                if (!suppress) { *va_arg(ap, unsigned int *) = (unsigned int)v; n++; }
            } else if (spec == 's') {
                char tmp[256]; int ti = 0;
                while (*s && *s!=' ' && *s!='\t' && *s!='\n' && ti<255)
                    tmp[ti++] = *s++;
                tmp[ti] = '\0';
                if (!ti) break;
                if (!suppress) { strcpy(va_arg(ap, char *), tmp); n++; }
            } else if (spec == 'c') {
                if (!suppress) { *va_arg(ap, char *) = *s; n++; }
                s++;
            } else if (spec == 'n') {
                if (!suppress) *va_arg(ap, int *) = (int)(s - (fmt)); /* approx */
            }
        } else if (*fmt == ' ') {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            fmt++;
        } else {
            if (*s != *fmt) break;
            s++; fmt++;
        }
    }
    return n;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap); return n;
}

int fscanf(FILE *f, const char *fmt, ...) {
    /* Simple: read a line and sscanf it */
    char line[512];
    if (!fgets(line, sizeof(line), f)) return EOF;
    va_list ap; va_start(ap, fmt);
    int n = vsscanf(line, fmt, ap);
    va_end(ap); return n;
}

int scanf(const char *fmt, ...) {
    char line[512];
    if (!fgets(line, sizeof(line), stdin)) return EOF;
    va_list ap; va_start(ap, fmt);
    int n = vsscanf(line, fmt, ap);
    va_end(ap); return n;
}

void perror(const char *s) {
    extern int errno;
    if (s && *s)
        fprintf(stderr, "%s: error %d\n", s, errno);
    else
        fprintf(stderr, "error %d\n", errno);
}

int remove(const char *path) {
    return unlink(path);
}

