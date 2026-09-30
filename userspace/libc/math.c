/* libm on the x87 FPU (userspace is built without SSE): the transcendental
 * instructions do the work, in 80-bit precision, rounded to double on return. */
#include "../include/math.h"

double fabs(double x) {
    union { double d; unsigned long long u; } v;
    v.d = x;
    v.u &= ~(1ULL << 63);
    return v.d;
}
float fabsf(float x) { return (float)fabs(x); }

double sqrt(double x) {
    long double r;
    __asm__("fsqrt" : "=t"(r) : "0"((long double)x));
    return (double)r;
}
float sqrtf(float x) { return (float)sqrt(x); }

/* fsin/fcos/fptan take |x| < 2^63; reduce larger arguments with fmod first. */
static long double trig_arg(double x) {
    if (x > 9.2e18 || x < -9.2e18) return fmod(x, 2 * M_PI);
    return x;
}
double sin(double x) {
    long double r;
    __asm__("fsin" : "=t"(r) : "0"(trig_arg(x)));
    return (double)r;
}
double cos(double x) {
    long double r;
    __asm__("fcos" : "=t"(r) : "0"(trig_arg(x)));
    return (double)r;
}
double tan(double x) {
    long double r, one;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"(trig_arg(x)));
    return (double)r;
}

double atan2(double y, double x) {
    long double r;
    __asm__("fpatan" : "=t"(r) : "0"((long double)x), "u"((long double)y) : "st(1)");
    return (double)r;
}
double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt((1.0 - x) * (1.0 + x))); }
double acos(double x) { return atan2(sqrt((1.0 - x) * (1.0 + x)), x); }

/* log2(x) * y, in extended precision. */
static long double yl2x(long double x, long double y) {
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double log(double x) {
    if (x < 0 || isnan(x)) return NAN;
    if (x == 0) return -HUGE_VAL;
    return (double)yl2x(x, 0.693147180559945309417232121458176568L);
}
double log2(double x) {
    if (x < 0 || isnan(x)) return NAN;
    if (x == 0) return -HUGE_VAL;
    return (double)yl2x(x, 1.0L);
}
double log10(double x) {
    if (x < 0 || isnan(x)) return NAN;
    if (x == 0) return -HUGE_VAL;
    return (double)yl2x(x, 0.301029995663981195213738894724493027L);
}
double log1p(double x) { return log(1.0 + x); }

/* 2^x: f2xm1 on the fraction, fscale by the integer part. */
static long double exp2l_(long double x) {
    long double i, f, r;
    if (x != x) return x;
    if (x > 16384) return __builtin_infl();
    if (x < -16500) return 0;
    __asm__("frndint" : "=t"(i) : "0"(x));
    f = x - i;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1.0L;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return r;
}

double exp2(double x) { return (double)exp2l_(x); }
double exp(double x) {
    return (double)exp2l_((long double)x * 1.44269504088896340735992468100189214L);
}
double expm1(double x) { return exp(x) - 1.0; }

double sinh(double x) { double e = exp(x); return (e - 1.0 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) / 2; }
double tanh(double x) {
    if (x > 20) return 1.0;
    if (x < -20) return -1.0;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}

double trunc(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;   /* NaN, inf, integral */
    return (double)(long long)x;
}
double floor(double x) {
    double t = trunc(x);
    return (t > x) ? t - 1.0 : t;
}
double ceil(double x) {
    double t = trunc(x);
    return (t < x) ? t + 1.0 : t;
}
double round(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
double rint(double x) {
    long double r;
    __asm__("frndint" : "=t"(r) : "0"((long double)x));
    return (double)r;
}
double nearbyint(double x) { return rint(x); }
long lrint(double x) { return (long)rint(x); }

double fmod(double x, double y) {
    long double r;
    unsigned short sw;
    if (y == 0 || isinf(x) || isnan(x) || isnan(y)) return NAN;
    r = x;
    do {
        __asm__("fprem; fnstsw %%ax" : "=t"(r), "=a"(sw) : "0"(r), "u"((long double)y));
    } while (sw & 0x400);       /* C2: reduction incomplete */
    return (double)r;
}

double modf(double x, double *iptr) {
    double i = trunc(x);
    *iptr = i;
    return isinf(x) ? 0.0 * x : x - i;
}

double ldexp(double x, int e) {
    long double r;
    __asm__("fscale" : "=t"(r) : "0"((long double)x), "u"((long double)e));
    return (double)r;
}
double scalbn(double x, int e) { return ldexp(x, e); }

double frexp(double x, int *e) {
    union { double d; unsigned long long u; } v = { x };
    int ex = (int)(v.u >> 52) & 0x7ff;
    if (!ex) {                          /* zero or subnormal */
        if (x == 0) { *e = 0; return x; }
        x = frexp(x * 18446744073709551616.0, e);
        *e -= 64;
        return x;
    }
    if (ex == 0x7ff) { *e = 0; return x; }
    *e = ex - 1022;
    v.u = (v.u & 0x800fffffffffffffULL) | 0x3fe0000000000000ULL;
    return v.d;
}

double pow(double x, double y) {
    if (y == 0) return 1.0;
    if (x == 1.0 || isnan(y)) return (x == 1.0) ? 1.0 : y;
    if (isnan(x)) return x;
    if (x == 0) {
        if (y < 0) return HUGE_VAL;
        return (y == trunc(y) && fmod(fabs(y), 2) == 1) ? x : 0.0;
    }
    int neg = 0;
    if (x < 0) {
        if (y != trunc(y)) return NAN;
        neg = fmod(fabs(y), 2) == 1;
        x = -x;
    }
    /* Small integral powers exactly by squaring. */
    if (y == trunc(y) && fabs(y) <= 1024) {
        long double r = 1, b = x;
        unsigned n = (unsigned)fabs(y);
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        if (y < 0) r = 1 / r;
        return neg ? -(double)r : (double)r;
    }
    long double r = exp2l_(yl2x(x, y));
    return neg ? -(double)r : (double)r;
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }
double cbrt(double x) { return x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3); }
double copysign(double x, double y) {
    return (__builtin_signbit(y) ? -fabs(x) : fabs(x));
}
double fmin(double x, double y) { return isnan(x) ? y : (isnan(y) || x < y) ? x : y; }
double fmax(double x, double y) { return isnan(x) ? y : (isnan(y) || x > y) ? x : y; }
