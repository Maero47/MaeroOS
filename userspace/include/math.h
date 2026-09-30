#pragma once

#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                           FP_SUBNORMAL, FP_ZERO, x)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

double fabs(double x);
float  fabsf(float x);
double sqrt(double x);
float  sqrtf(float x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double hypot(double x, double y);
double cbrt(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double rint(double x);
double nearbyint(double x);
long   lround(double x);
long long llround(double x);
long   lrint(double x);
double fmod(double x, double y);
double modf(double x, double *iptr);
double ldexp(double x, int e);
double scalbn(double x, int e);
double frexp(double x, int *e);
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
