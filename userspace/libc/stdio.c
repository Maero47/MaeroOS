#include "libc_lock.h"
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include <stdarg.h>
#include <stdint.h>
#include "../include/syscall.h"
#include "../include/errno.h"

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

/* Write out the whole buffer, retrying short writes.  A failed or zero-length
 * write marks the stream in error and returns EOF so callers (fclose, fwrite,
 * fflush) can report it — silently dropping it lets a writer replace a file
 * with a truncated copy and believe it succeeded. */
/* Every FILE's buffers and the stdout/stderr order are guarded by one lock:
 * two threads printing at once must not both advance wpos past the buffer.
 * Public entry points take it; the _unlocked helpers expect it held. */
volatile int __libc_stdio_lock;
#define STDIO_LOCK()   libc_lock(&__libc_stdio_lock)
#define STDIO_UNLOCK() libc_unlock(&__libc_stdio_lock)

static int _fflush_unlocked(FILE *f) {
    int off = 0;
    while (off < f->wpos) {
        int w = write(f->fd, f->wbuf + off, f->wpos - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            f->error = 1;
            f->wpos = 0;
            return EOF;
        }
        off += w;
    }
    f->wpos = 0;
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
    int fd = open(path, flags, 0666);
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
    if (!f) return EOF;
    STDIO_LOCK();
    int r = _fflush_unlocked(f);
    if (f->error && f->mode != FILE_RDONLY) r = EOF;   /* an earlier write failed */
    if (close(f->fd) < 0) r = EOF;
    STDIO_UNLOCK();
    if (f != stdin && f != stdout && f != stderr)
        free(f);
    return r;
}

int fflush(FILE *f) {
    int r;
    STDIO_LOCK();
    if (!f) {
        /* fflush(NULL): only the standard streams are tracked. */
        r = _fflush_unlocked(stdout);
        if (_fflush_unlocked(stderr)) r = EOF;
    } else {
        r = _fflush_unlocked(f);
    }
    STDIO_UNLOCK();
    return r;
}

/* ── fread / fwrite ──────────────────────────────────────────────────────────── */

static size_t _fwrite_unlocked(const void *ptr, size_t sz, size_t n, FILE *f);

size_t fwrite(const void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f) return 0;
    STDIO_LOCK();
    size_t r = _fwrite_unlocked(ptr, sz, n, f);
    STDIO_UNLOCK();
    return r;
}

static size_t _fwrite_unlocked(const void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f || f->error || !sz || !n) return 0;
    if (n > (size_t)-1 / sz) { f->error = 1; return 0; }
    size_t total = sz * n;
    size_t done = 0;   /* bytes of this call known to have reached write() */
    const char *p = ptr;
    /* Line-buffered: buffer until newline or full, then flush */
    for (size_t i = 0; i < total; i++) {
        f->wbuf[f->wpos++] = p[i];
        if (f->wpos == FILE_BUFSZ || p[i] == '\n') {
            if (_fflush_unlocked(f)) return done / sz;
            done = i + 1;
        }
    }
    return n;
}

static size_t _fread_unlocked(void *ptr, size_t sz, size_t n, FILE *f);

size_t fread(void *ptr, size_t sz, size_t n, FILE *f) {
    if (!f) return 0;
    STDIO_LOCK();
    size_t r = _fread_unlocked(ptr, sz, n, f);
    STDIO_UNLOCK();
    return r;
}

static size_t _fread_unlocked(void *ptr, size_t sz, size_t n, FILE *f) {
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
    if (!f) return EOF;
    STDIO_LOCK();
    int r = (unsigned char)c;
    if (f->error) {
        r = EOF;
    } else {
        char ch = (char)c;
        f->wbuf[f->wpos++] = ch;
        if ((f->wpos == FILE_BUFSZ || ch == '\n') && _fflush_unlocked(f))
            r = EOF;
    }
    STDIO_UNLOCK();
    return r;
}

static int _fgetc_unlocked(FILE *f) {
    if (f->error || f->eof) return EOF;
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->rpos >= f->rlen) {
        int r = read(f->fd, f->rbuf, FILE_BUFSZ);
        if (r <= 0) { if (r == 0) f->eof = 1; else f->error = 1; return EOF; }
        f->rpos = 0; f->rlen = r;
    }
    return (unsigned char)f->rbuf[f->rpos++];
}

int fgetc(FILE *f) {
    if (!f) return EOF;
    STDIO_LOCK();
    int c = _fgetc_unlocked(f);
    STDIO_UNLOCK();
    return c;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    STDIO_LOCK();
    f->unget = (unsigned char)c;
    f->eof   = 0;
    STDIO_UNLOCK();
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
    STDIO_LOCK();
    _fflush_unlocked(f);
    f->rpos = f->rlen = 0;
    f->unget = -1;
    f->eof   = 0;
    int r = lseek(f->fd, (int)offset, whence) < 0 ? -1 : 0;
    STDIO_UNLOCK();
    return r;
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

/* *n /= base, returns the remainder.  Done by hand so libc does not need
 * libgcc's __udivdi3/__umoddi3 (most programs link without -lgcc). */
static unsigned _divmod_u64(unsigned long long *n, unsigned base) {
    unsigned hi = (unsigned)(*n >> 32), lo = (unsigned)*n;
    unsigned long long q;
    unsigned r;
    if (!hi) { *n = lo / base; return lo % base; }
    q = (unsigned long long)(hi / base) << 32;
    r = hi % base;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((lo >> i) & 1);   /* r < base <= 16, no overflow */
        if (r >= base) { r -= base; q |= 1ULL << i; }
    }
    *n = q;
    return r;
}

/* ── floating point conversions ─────────────────────────────────────────── */

static long double _pow10l(int e) {
    long double r = 1.0L, b = 10.0L;
    unsigned n = e < 0 ? -(unsigned)e : (unsigned)e;
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1.0L / r : r;
}

/* Decimal digits of v > 0 (or 0): mode 'e' gives prec+1 significant digits,
 * mode 'f' every digit down to 10^-prec.  Returns the digit count (at most
 * `cap`, zeros past the 19 that a long double holds) and the decimal exponent
 * of the first digit in *e10. */
static int _fdigits(long double v, int mode, int prec, char *dig, int cap, int *e10) {
    int e = 0, nd, k;
    unsigned long long u, lim;

    if (v == 0) {
        *e10 = 0;
        nd = mode == 'e' ? prec + 1 : prec + 1;
        if (nd > cap) nd = cap;
        for (int i = 0; i < nd; i++) dig[i] = '0';
        return nd;
    }
    /* Normalise v = m * 10^e with 1 <= m < 10. */
    long double m = v;
    if (m >= 10.0L || m < 1.0L) {
        int guess = 0;
        long double t = m;
        while (t >= 1e32L) { t /= 1e32L; guess += 32; }
        while (t < 1e-32L) { t *= 1e32L; guess -= 32; }
        while (t >= 10.0L) { t /= 10.0L; guess++; }
        while (t < 1.0L) { t *= 10.0L; guess--; }
        e = guess;
        m = v * _pow10l(-e);
        if (e < -4900) m = t;
        while (m >= 10.0L) { m /= 10.0L; e++; }
        while (m < 1.0L) { m *= 10.0L; e--; }
    }
    nd = mode == 'e' ? prec + 1 : e + 1 + prec;
    if (nd <= 0) {
        /* Everything is below the last printed place: rounds to 0 or 1 unit. */
        *e10 = e;
        if (nd == 0 && m > 5.0L) { dig[0] = '1'; *e10 = e + 1; return 1; }
        dig[0] = '0';
        *e10 = -prec - 1;
        return 0;
    }
    if (nd > cap) nd = cap;
    k = nd < 19 ? nd : 19;
    lim = 1;
    for (int i = 0; i < k; i++) lim *= 10;
    long double scaled = m * _pow10l(k - 1);
    u = (unsigned long long)scaled;
    long double rem = scaled - (long double)u;
    if (rem > 0.5L || (rem == 0.5L && (u & 1))) u++;     /* ties to even */
    if (u >= lim) {             /* 9.99 -> 10.0: one more digit to the left */
        u /= 10;
        e++;
        if (mode != 'e' && nd < cap) { nd++; if (k < 19) { k++; lim *= 10; u *= 10; } }
    }
    for (int i = k - 1; i >= 0; i--) { dig[i] = (char)('0' + u % 10); u /= 10; }
    for (int i = k; i < nd; i++) dig[i] = '0';
    *e10 = e;
    return nd;
}

/* Format v by a %e/%f/%g conversion into out (NUL-terminated, no sign). */
static int _ffmt(char *out, int cap, long double v, char spec, int prec, int alt) {
    char dig[400];
    int e10, nd, n = 0, lower = spec >= 'a';
    char c = spec | 32;

#define PUT(ch) do { if (n < cap - 1) out[n++] = (ch); } while (0)
    if (v != v) { const char *t = lower ? "nan" : "NAN"; while (*t) PUT(*t++); out[n] = 0; return n; }
    if (v > 1e4932L) { const char *t = lower ? "inf" : "INF"; while (*t) PUT(*t++); out[n] = 0; return n; }
    if (prec < 0) prec = 6;
    if (c == 'g') {
        int P = prec ? prec : 1, X;
        _fdigits(v, 'e', P - 1, dig, sizeof(dig), &X);
        if (v == 0) X = 0;
        if (P > X && X >= -4) { c = 'f'; prec = P - 1 - X; }
        else { c = 'e'; prec = P - 1; }
        if (!alt) {
            /* %g drops trailing zeros: format, then trim. */
            int len = _ffmt(out, cap, v, lower ? c : c - 32, prec, 0);
            char *dot = 0, *ep = 0;
            for (int i = 0; i < len; i++) {
                if (out[i] == '.') dot = out + i;
                if ((out[i] | 32) == 'e') ep = out + i;
            }
            if (!dot) return len;
            char *end = ep ? ep : out + len, *z = end;
            while (z > dot + 1 && z[-1] == '0') z--;
            if (z == dot + 1) z = dot;
            int tail = (int)(out + len - end);
            for (int i = 0; i <= tail; i++) z[i] = end[i];
            return (int)(z - out) + tail;
        }
    }
    if (c == 'e') {
        nd = _fdigits(v, 'e', prec, dig, sizeof(dig), &e10);
        if (v == 0) e10 = 0;
        PUT(dig[0]);
        if (prec || alt) PUT('.');
        for (int i = 1; i < nd; i++) PUT(dig[i]);
        for (int i = nd; i < prec + 1; i++) PUT('0');
        PUT(lower ? 'e' : 'E');
        PUT(e10 < 0 ? '-' : '+');
        int ae = e10 < 0 ? -e10 : e10;
        if (ae >= 1000) PUT('0' + ae / 1000);
        if (ae >= 100) PUT('0' + ae / 100 % 10);
        PUT('0' + ae / 10 % 10);
        PUT('0' + ae % 10);
    } else {
        nd = _fdigits(v, 'f', prec, dig, sizeof(dig), &e10);
        if (v == 0) e10 = 0;
        if (e10 < 0) PUT('0');
        else for (int i = 0; i <= e10; i++) PUT(i < nd ? dig[i] : '0');
        if (prec || alt) PUT('.');
        for (int j = 1; j <= prec; j++) {
            int idx = e10 + j;
            PUT(idx >= 0 && idx < nd ? dig[idx] : '0');
        }
    }
#undef PUT
    out[n] = 0;
    return n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;

/* The argument is evaluated exactly once, even when nothing is stored
 * (cap == 0 is the length query): callers pass things like *fmt++. */
#define OUT(c) do { char _oc = (char)(c); if (cap && pos < cap - 1) buf[pos] = _oc; pos++; } while(0)

    while (*fmt) {
        if (*fmt != '%') { OUT(*fmt++); continue; }
        fmt++; /* skip % */

        /* Flags */
        int flag_zero = 0, flag_left = 0, flag_plus = 0, flag_space = 0, flag_alt = 0;
        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            if (*fmt == '#') flag_alt = 1;
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
        int is_long = 0, is_llong = 0, is_ldbl = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_llong = 1; fmt++; } }
        else if (*fmt == 'L' || *fmt == 'q') { is_ldbl = 1; is_llong = 1; fmt++; }
        else if (*fmt == 't') { is_long = 1; fmt++; }
        else if (*fmt == 'j') { is_llong = 1; fmt++; }
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

        if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' ||
            spec == 'g' || spec == 'G') {
            long double v = is_ldbl ? va_arg(ap, long double) : (long double)va_arg(ap, double);
            char fbuf[512], sign = 0;
            if (__builtin_signbit(v)) { sign = '-'; v = -v; }
            else if (flag_plus) sign = '+';
            else if (flag_space) sign = ' ';
            int flen = _ffmt(fbuf, sizeof(fbuf), v, spec, prec, flag_alt);
            int pad = width - flen - (sign ? 1 : 0);
            if (v != v || v > 1e4932L) flag_zero = 0;
            if (!flag_left && !flag_zero) while (pad-- > 0) OUT(' ');
            if (sign) OUT(sign);
            if (!flag_left && flag_zero) while (pad-- > 0) OUT('0');
            for (int i = 0; i < flen; i++) OUT(fbuf[i]);
            if (flag_left) while (pad-- > 0) OUT(' ');
            continue;
        }

        /* Numeric conversions.  64-bit throughout: %lld/%llu consume a whole
         * long long vararg, everything else is widened from int/long. */
        char nbuf[32]; int nlen = 0;
        unsigned long long uval = 0;
        int is_signed = 0, negative = 0;
        int base = 10;
        int upper = 0;
        char prefix[3] = {0,0,0};

        switch (spec) {
        case 'd': case 'i':
            is_signed = 1;
            { long long v = is_llong ? va_arg(ap, long long)
                          : is_long ? (long long)va_arg(ap, long)
                          : (long long)va_arg(ap, int);
              if (v < 0) { negative = 1; uval = 0ULL - (unsigned long long)v; }
              else uval = (unsigned long long)v; }
            break;
        case 'u': case 'o': case 'x': case 'X':
            if (spec == 'o') base = 8;
            if (spec == 'x' || spec == 'X') base = 16;
            if (spec == 'X') upper = 1;
            uval = is_llong ? va_arg(ap, unsigned long long)
                 : is_long ? (unsigned long long)va_arg(ap, unsigned long)
                 : (unsigned long long)va_arg(ap, unsigned int);
            if (flag_alt && uval && base == 16) { prefix[0] = '0'; prefix[1] = spec; }
            if (flag_alt && uval && base == 8) prefix[0] = '0';
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
        else { while (uval) { nbuf[nlen++] = digits[_divmod_u64(&uval, (unsigned)base)]; } }

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

/* Format into a stack buffer, or a heap one when the output is longer. */
static char *_vformat(char *stackbuf, size_t cap, int *len, const char *fmt, va_list ap) {
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(stackbuf, cap, fmt, ap);
    char *out = stackbuf;
    if (n >= (int)cap) {
        out = malloc((size_t)n + 1);
        if (out) vsnprintf(out, (size_t)n + 1, fmt, aq);
        else { out = stackbuf; n = (int)cap - 1; }
    }
    va_end(aq);
    *len = n;
    return out;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n;
    char *out = _vformat(buf, sizeof(buf), &n, fmt, ap);
    int off = 0;
    /* printf writes at once, but whatever putchar/fputs left in stdout's
     * buffer has to go out first to keep the order. */
    STDIO_LOCK();
    _fflush_unlocked(stdout);
    while (off < n) {
        int w = write(1, out + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { n = -1; break; }
        off += w;
    }
    STDIO_UNLOCK();
    if (out != buf) free(out);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap); return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f) return vprintf(fmt, ap);
    char buf[1024];
    int n;
    char *out = _vformat(buf, sizeof(buf), &n, fmt, ap);
    if (n > 0 && fwrite(out, 1, (size_t)n, f) != (size_t)n) n = -1;
    if (out != buf) free(out);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
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

int vasprintf(char **strp, const char *fmt, va_list ap) {
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf((char *)0, 0, fmt, aq);
    va_end(aq);
    *strp = n < 0 ? (char *)0 : malloc((size_t)n + 1);
    if (!*strp) return -1;
    vsnprintf(*strp, (size_t)n + 1, fmt, ap);
    return n;
}

int asprintf(char **strp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vasprintf(strp, fmt, ap);
    va_end(ap);
    return n;
}

/* ── putchar / puts ──────────────────────────────────────────────────────────── */

int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { if (fputs(s, stdout) == EOF) return EOF; return fputc('\n', stdout); }
int putc(int c, FILE *f) { return fputc(c, f); }
int getc(FILE *f) { return fgetc(f); }

int getchar(void) {
    char c;
    int r = read(0, &c, 1);
    if (r <= 0) return EOF;
    return (unsigned char)c;
}

/* ── sscanf ──────────────────────────────────────────────────────────────────── */

static int _isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static int _digit(int c, int base) {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10
          : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
}

/* C99 sscanf: whitespace, literals, %% and the d i u o x X p n s c [ and
 * floating conversions, with assignment suppression, widths and the
 * hh h l ll L j z t q modifiers. */
int vsscanf(const char *str, const char *fmt, va_list ap) {
    const unsigned char *s = (const unsigned char *)str;
    int n = 0;

    while (*fmt) {
        if (_isspace((unsigned char)*fmt)) {
            while (_isspace(*s)) s++;
            fmt++;
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') {
                fmt++;
                while (_isspace(*s)) s++;
            }
            if (*s != (unsigned char)*fmt) return (!*s && !n) ? EOF : n;
            s++;
            fmt++;
            continue;
        }
        fmt++;
        int suppress = 0, width = 0, lmod = 0;   /* -2 hh, -1 h, 1 l, 2 ll, 3 L */
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'h') { lmod = -1; fmt++; if (*fmt == 'h') { lmod = -2; fmt++; } }
        else if (*fmt == 'l') { lmod = 1; fmt++; if (*fmt == 'l') { lmod = 2; fmt++; } }
        else if (*fmt == 'L') { lmod = 3; fmt++; }
        else if (*fmt == 'j' || *fmt == 'q') { lmod = 2; fmt++; }
        else if (*fmt == 'z' || *fmt == 't') { lmod = 1; fmt++; }
        char spec = *fmt++;
        if (!spec) break;

        if (spec == 'n') {
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                long long v = (const char *)s - str;
                if (lmod == 2) *(long long *)dst = v;
                else if (lmod == -1) *(short *)dst = (short)v;
                else if (lmod == -2) *(signed char *)dst = (signed char)v;
                else *(int *)dst = (int)v;
            }
            continue;
        }
        if (spec != 'c' && spec != '[') while (_isspace(*s)) s++;
        if (!*s) return n ? n : EOF;
        int left = width ? width : 0x7fffffff;

        if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' ||
            spec == 'x' || spec == 'X' || spec == 'p') {
            int base = spec == 'o' ? 8 : (spec == 'x' || spec == 'X' || spec == 'p') ? 16
                     : spec == 'i' ? 0 : 10, neg = 0, got = 0, d;
            unsigned long long v = 0;
            if (left && (*s == '-' || *s == '+')) { neg = *s == '-'; s++; left--; }
            if ((base == 0 || base == 16) && left >= 2 && s[0] == '0' && (s[1] | 32) == 'x'
                && _digit(s[2], 16) >= 0) {
                s += 2;
                left -= 2;
                base = 16;
            } else if (base == 0) base = (*s == '0') ? 8 : 10;
            while (left && (d = _digit(*s, base)) >= 0) {
                v = v * (unsigned)base + (unsigned)d;
                s++;
                left--;
                got = 1;
            }
            if (!got) return n;
            if (neg) v = 0ULL - v;
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (spec == 'p') *(void **)dst = (void *)(uintptr_t)v;
                else if (lmod == 2) *(unsigned long long *)dst = v;
                else if (lmod == -1) *(unsigned short *)dst = (unsigned short)v;
                else if (lmod == -2) *(unsigned char *)dst = (unsigned char)v;
                else if (lmod == 1) *(unsigned long *)dst = (unsigned long)v;
                else *(unsigned int *)dst = (unsigned int)v;
                n++;
            }
        } else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' ||
                   spec == 'g' || spec == 'G' || spec == 'a' || spec == 'A') {
            char buf[128], *end;
            int len = 0;
            while (len < left && len < (int)sizeof(buf) - 1 && s[len] && !_isspace(s[len]))
                buf[len] = (char)s[len], len++;
            buf[len] = 0;
            long double v = strtold(buf, &end);
            if (end == buf) return n;
            s += end - buf;
            if (!suppress) {
                void *dst = va_arg(ap, void *);
                if (lmod == 3) *(long double *)dst = v;
                else if (lmod == 1) *(double *)dst = (double)v;
                else *(float *)dst = (float)v;
                n++;
            }
        } else if (spec == 's') {
            /* A bare %s keeps the historical 255-character cap. */
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int ti = 0;
            if (!width) left = 255;
            while (*s && !_isspace(*s) && ti < left) {
                if (dst) dst[ti] = (char)*s;
                ti++;
                s++;
            }
            if (dst) { dst[ti] = '\0'; n++; }
        } else if (spec == 'c') {
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            int want = width ? width : 1, ti = 0;
            for (; ti < want && *s; ti++, s++) if (dst) dst[ti] = (char)*s;
            if (ti < want) return n;
            if (dst) n++;
        } else if (spec == '[') {
            unsigned char set[32];
            int neg = 0, ti = 0;
            memset(set, 0, sizeof(set));
            if (*fmt == '^') { neg = 1; fmt++; }
            if (*fmt == ']') { set[']' >> 3] |= 1 << (']' & 7); fmt++; }
            while (*fmt && *fmt != ']') {
                int lo = (unsigned char)*fmt++;
                if (*fmt == '-' && fmt[1] && fmt[1] != ']') {
                    int hi = (unsigned char)fmt[1];
                    for (int c = lo; c <= hi; c++) set[c >> 3] |= 1 << (c & 7);
                    fmt += 2;
                } else set[lo >> 3] |= 1 << (lo & 7);
            }
            if (*fmt == ']') fmt++;
            char *dst = suppress ? (char *)0 : va_arg(ap, char *);
            while (*s && ti < left && (!!(set[*s >> 3] & (1 << (*s & 7))) != neg)) {
                if (dst) dst[ti] = (char)*s;
                ti++;
                s++;
            }
            if (!ti) return n;
            if (dst) { dst[ti] = '\0'; n++; }
        } else return n;
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

/* C remove(): a file with unlink(), a directory with rmdir() (unlink says
 * EISDIR for one, as on Linux). */
int remove(const char *path) {
    int r = unlink(path);
    if (r < 0 && errno == EISDIR)
        r = rmdir(path);
    return r;
}

