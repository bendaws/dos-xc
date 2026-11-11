/*
 * DOS-XC /os/libc/src/math.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "libc.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

int signgam;

#define LN2L 0.693147180559945309417232121458176568L
#define LOG2EL 1.44269504088896340735992468100189214L
#define PIL 3.14159265358979323846264338327950288L

typedef union { double d; uint64_t u; } double_bits;
typedef union { float f; uint32_t u; } float_bits;
typedef union { long double l; struct { uint64_t mantissa; uint16_t exponent; } parts; } long_bits;

/* ---- x87 primitives ---- */
static long double x87_round(long double x, unsigned short mode) {
    unsigned short old, cw;
    __asm__ volatile("fnstcw %0" : "=m"(old));
    cw = (unsigned short)((old & ~0x0c00) | mode);
    __asm__ volatile("fldcw %0" : : "m"(cw));
    __asm__ volatile("frndint" : "+t"(x));
    __asm__ volatile("fldcw %0" : : "m"(old));
    return x;
}
long double rintl(long double x) { __asm__ volatile("frndint" : "+t"(x)); return x; }
long double nearbyintl(long double x) { return rintl(x); }
long double floorl(long double x) { return x87_round(x, 0x0400); }
long double ceill(long double x) { return x87_round(x, 0x0800); }
long double truncl(long double x) { return x87_round(x, 0x0c00); }
long double roundl(long double x) {
    long double t = truncl(x), d = x - t;
    if (d >= 0.5L) t += 1;
    else if (d <= -0.5L) t -= 1;
    return t;
}

long double sqrtl(long double x) { __asm__("fsqrt" : "+t"(x)); return x; }
long double fabsl(long double x) { return __builtin_fabsl(x); }
long double copysignl(long double x, long double y) { return __builtin_copysignl(x, y); }

long double scalbnl(long double x, int n) {
    long double r;
    long double scale = (long double)n;
    __asm__("fscale" : "=t"(r) : "0"(x), "u"(scale));
    return r;
}
long double scalblnl(long double x, long n) { return scalbnl(x, n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)n); }
long double ldexpl(long double x, int n) { return scalbnl(x, n); }

long double frexpl(long double x, int *e) {
    if (x == 0 || __builtin_isnan(x) || __builtin_isinf(x)) {
        *e = 0;
        return x;
    }
    long_bits b = {x};
    int exponent = b.parts.exponent & 0x7fff;
    if (!exponent) {
        b.l *= 0x1p64L;
        exponent = (b.parts.exponent & 0x7fff) - 64;
    }
    *e = exponent - 16382;
    b.parts.exponent = (uint16_t)((b.parts.exponent & 0x8000) | 16382);
    return b.l;
}

long double fmodl(long double x, long double y) {
    unsigned short status;
    do __asm__("fprem; fnstsw %%ax" : "+t"(x), "=a"(status) : "u"(y));
    while (status & 0x400);
    return x;
}
long double remainderl(long double x, long double y) {
    unsigned short status;
    do __asm__("fprem1; fnstsw %%ax" : "+t"(x), "=a"(status) : "u"(y));
    while (status & 0x400);
    return x;
}
long double remquol(long double x, long double y, int *quotient) {
    long double r = remainderl(x, y);
    long double q = rintl((x - r) / y);
    *quotient = (int)fmodl(q, 0x1p31L);
    return r;
}

static long double atan2_x87(long double y, long double x) {
    long double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}
static long double log2_x87(long double x, long double scale) {
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(scale) : "st(1)");
    return r;
}

/* Bring |x| into the x87 trigonometric range (|x| < 2^63). */
static long double reduce(long double x) {
    if (__builtin_fabsl(x) < 0x1p62L) return x;
    return remainderl(x, 2 * PIL);
}
long double sinl(long double x) {
    if (__builtin_isinf(x)) return __builtin_nanl("");
    x = reduce(x);
    __asm__("fsin" : "+t"(x));
    return x;
}
long double cosl(long double x) {
    if (__builtin_isinf(x)) return __builtin_nanl("");
    x = reduce(x);
    __asm__("fcos" : "+t"(x));
    return x;
}
long double tanl(long double x) {
    if (__builtin_isinf(x)) return __builtin_nanl("");
    long double one;
    x = reduce(x);
    __asm__("fptan" : "=t"(one), "=u"(x) : "0"(x));
    return x;
}
void sincosl(long double x, long double *s, long double *c) {
    *s = sinl(x);
    *c = cosl(x);
}
long double atan2l(long double y, long double x) { return atan2_x87(y, x); }
long double atanl(long double x) { return atan2_x87(x, 1); }
long double asinl(long double x) {
    if (x > 1 || x < -1) return __builtin_nanl("");
    return atan2_x87(x, sqrtl((1 - x) * (1 + x)));
}
long double acosl(long double x) {
    if (x > 1 || x < -1) return __builtin_nanl("");
    return atan2_x87(sqrtl((1 - x) * (1 + x)), x);
}

long double exp2l(long double t) {
    if (__builtin_isnan(t)) return t;
    if (t > 16384) return __builtin_infl();
    if (t < -16500) return 0;
    long double n = rintl(t), f = t - n, r;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    return scalbnl(r + 1, (int)n);
}
long double expl(long double x) {
    if (__builtin_isnan(x)) return x;
    if (x > 11400) return __builtin_infl();
    if (x < -11500) return 0;
    long double hi = (long double)(float)x, lo = x - hi;
    long double t = hi * LOG2EL, n = rintl(t), f = (t - n) + lo * LOG2EL, r;
    if (f > 1 || f < -1) {
        long double extra = rintl(f);
        n += extra;
        f -= extra;
    }
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    return scalbnl(r + 1, (int)n);
}
long double expm1l(long double x) {
    long double t = x * LOG2EL;
    if (t > -1 && t < 1) {
        long double r;
        __asm__("f2xm1" : "=t"(r) : "0"(t));
        return r;
    }
    return expl(x) - 1;
}
long double exp10l(long double x) { return exp2l(x * 3.32192809488736234787031942948939018L); }

long double logl(long double x) {
    if (x < 0 || __builtin_isnan(x)) return __builtin_nanl("");
    if (x == 0) return -__builtin_infl();
    return log2_x87(x, LN2L);
}
long double log2l(long double x) {
    if (x < 0 || __builtin_isnan(x)) return __builtin_nanl("");
    if (x == 0) return -__builtin_infl();
    return log2_x87(x, 1);
}
long double log10l(long double x) {
    if (x < 0 || __builtin_isnan(x)) return __builtin_nanl("");
    if (x == 0) return -__builtin_infl();
    return log2_x87(x, 0.301029995663981195213738894724493027L);
}
long double log1pl(long double x) {
    if (x < -1 || __builtin_isnan(x)) return __builtin_nanl("");
    if (x == -1) return -__builtin_infl();
    if (__builtin_fabsl(x) < 0.29L) {
        long double r, scale = LN2L;
        __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(scale) : "st(1)");
        return r;
    }
    return logl(1 + x);
}
long double logbl(long double x) {
    if (x == 0) return -__builtin_infl();
    if (__builtin_isinf(x)) return __builtin_infl();
    if (__builtin_isnan(x)) return x;
    int e;
    frexpl(x, &e);
    return (long double)(e - 1);
}
int ilogbl(long double x) {
    if (x == 0) return FP_ILOGB0;
    if (__builtin_isnan(x)) return FP_ILOGBNAN;
    if (__builtin_isinf(x)) return INT_MAX;
    return (int)logbl(x);
}

static int is_integer(long double y) { return __builtin_isfinite(y) && truncl(y) == y; }

long double powl(long double x, long double y) {
    if (y == 0 || x == 1) return 1;
    if (__builtin_isnan(x) || __builtin_isnan(y)) return x + y;
    if (is_integer(y) && __builtin_fabsl(y) <= 1024 && x != 0 && __builtin_isfinite(x)) {
        unsigned long long n = (unsigned long long)__builtin_fabsl(y);
        long double result = 1, base = x;
        while (n) {
            if (n & 1) result *= base;
            base *= base;
            n >>= 1;
        }
        return y < 0 ? 1 / result : result;
    }
    if (x == 0) {
        int odd = is_integer(y) && fmodl(y, 2) != 0;
        if (y < 0) return odd ? copysignl(__builtin_infl(), x) : __builtin_infl();
        return odd ? x : 0;
    }
    if (__builtin_isinf(y)) {
        long double ax = __builtin_fabsl(x);
        if (ax == 1) return 1;
        return (ax > 1) == (y > 0) ? __builtin_infl() : 0;
    }
    if (__builtin_isinf(x)) {
        int odd = is_integer(y) && fmodl(y, 2) != 0;
        long double r = y > 0 ? __builtin_infl() : 0;
        return x < 0 && odd ? -r : r;
    }
    long double sign = 1;
    if (x < 0) {
        if (!is_integer(y)) return __builtin_nanl("");
        if (fmodl(y, 2) != 0) sign = -1;
        x = -x;
    }
    return sign * exp2l(y * log2l(x));
}

long double cbrtl(long double x) {
    if (x == 0 || !__builtin_isfinite(x)) return x;
    long double a = __builtin_fabsl(x), r = exp2l(log2l(a) / 3);
    r = r - (r * r * r - a) / (3 * r * r);
    return x < 0 ? -r : r;
}
long double hypotl(long double x, long double y) {
    if (__builtin_isinf(x) || __builtin_isinf(y)) return __builtin_infl();
    return sqrtl(x * x + y * y);
}

long double sinhl(long double x) {
    if (__builtin_fabsl(x) < 0x1p-32L) return x;
    long double e = expm1l(__builtin_fabsl(x));
    long double r = (e + e / (e + 1)) / 2;
    return x < 0 ? -r : r;
}
long double coshl(long double x) {
    long double e = expl(__builtin_fabsl(x));
    return (e + 1 / e) / 2;
}
long double tanhl(long double x) {
    if (__builtin_fabsl(x) > 45) return x < 0 ? -1 : 1;
    long double e = expm1l(2 * __builtin_fabsl(x));
    long double r = e / (e + 2);
    return x < 0 ? -r : r;
}
long double asinhl(long double x) {
    long double a = __builtin_fabsl(x);
    long double r = a > 0x1p32L ? logl(a) + LN2L : log1pl(a + a * a / (1 + sqrtl(1 + a * a)));
    return x < 0 ? -r : r;
}
long double acoshl(long double x) {
    if (x < 1) return __builtin_nanl("");
    if (x > 0x1p32L) return logl(x) + LN2L;
    return logl(x + sqrtl(x * x - 1));
}
long double atanhl(long double x) {
    if (x > 1 || x < -1) return __builtin_nanl("");
    return 0.5L * log1pl(2 * x / (1 - x));
}

/* erf: Taylor series near zero, continued fraction for erfc further out. */
long double erfcl(long double x);
long double erfl(long double x) {
    long double a = __builtin_fabsl(x);
    if (a > 6) return x < 0 ? -1 : 1;
    if (a >= 2.5L) return x < 0 ? -(1 - erfcl(a)) : 1 - erfcl(a);
    long double term = a, sum = a, x2 = a * a;
    for (int n = 1; n < 200; ++n) {
        term *= -x2 / n;
        long double add = term / (2 * n + 1);
        sum += add;
        if (__builtin_fabsl(add) < 1e-21L * __builtin_fabsl(sum)) break;
    }
    sum *= 1.12837916709551257389615890312154517L;
    return x < 0 ? -sum : sum;
}
long double erfcl(long double x) {
    if (x < 2.5L) return 1 - erfl(x);
    if (x > 107) return 0;
    long double f = x, c = x, d = 0, tiny = 1e-4000L;
    for (int n = 1; n < 500; ++n) {
        long double an = n * 0.5L;
        d = x + an * d;
        if (d == 0) d = tiny;
        c = x + an / c;
        if (c == 0) c = tiny;
        d = 1 / d;
        long double delta = c * d;
        f *= delta;
        if (__builtin_fabsl(delta - 1) < 1e-20L) break;
    }
    return expl(-x * x) / (f * 1.77245385090551602729816748334114518L);
}

static const long double lanczos[] = {
    0.99999999999980993227684700473478L, 676.520368121885098567009190444019L,
    -1259.13921672240287047156078755283L, 771.3234287776530788486528258894L,
    -176.61502916214059906584551354L, 12.507343278686904814458936853L,
    -0.13857109526572011689554707L, 9.984369578019570859563e-6L, 1.50563273514931155834e-7L};
static long double lanczos_log_gamma(long double x) {
    x -= 1;
    long double a = lanczos[0], t = x + 7.5L;
    for (int i = 1; i < 9; ++i) a += lanczos[i] / (x + i);
    return 0.918938533204672741780329736405617639L + (x + 0.5L) * logl(t) - t + logl(a);
}
long double lgammal(long double x) {
    signgam = 1;
    if (__builtin_isinf(x)) return __builtin_infl();
    if (x <= 0 && is_integer(x)) return __builtin_infl();
    if (x < 0.5L) {
        long double s = sinl(PIL * x);
        if (s < 0) signgam = -1;
        return logl(PIL / __builtin_fabsl(s)) - lanczos_log_gamma(1 - x);
    }
    return lanczos_log_gamma(x);
}
long double tgammal(long double x) {
    if (x == 0) return copysignl(__builtin_infl(), x);
    if (x < 0 && is_integer(x)) return __builtin_nanl("");
    if (x > 1755.5L) return __builtin_infl();
    if (is_integer(x) && x <= 30) {
        long double r = 1;
        for (int i = 2; i < (int)x; ++i) r *= i;
        return r;
    }
    long double l = lgammal(x);
    return signgam * expl(l);
}

long double nanl(const char *tag) { (void)tag; return __builtin_nanl(""); }
long double fdiml(long double x, long double y) { return x > y ? x - y : (__builtin_isnan(x) || __builtin_isnan(y)) ? x + y : 0; }
long double fmaxl(long double x, long double y) { return __builtin_isnan(x) ? y : __builtin_isnan(y) ? x : x > y ? x : y; }
long double fminl(long double x, long double y) { return __builtin_isnan(x) ? y : __builtin_isnan(y) ? x : x < y ? x : y; }
long double fmal(long double x, long double y, long double z) { return x * y + z; }
long double modfl(long double x, long double *integer) {
    *integer = truncl(x);
    if (__builtin_isinf(x)) return copysignl(0, x);
    return copysignl(x - *integer, x);
}
long double nextafterl(long double x, long double y) {
    if (__builtin_isnan(x) || __builtin_isnan(y)) return x + y;
    if (x == y) return y;
    if (x == 0) {
        long_bits b = {0};
        b.parts.mantissa = 1;
        b.parts.exponent = y < 0 ? 0x8000 : 0;
        return b.l;
    }
    long_bits b = {x};
    int up = (x < y) == (x > 0);
    if (up) {
        if (++b.parts.mantissa == 0) {
            b.parts.mantissa = 0x8000000000000000ull;
            ++b.parts.exponent;
        } else if (b.parts.mantissa == 0x8000000000000000ull && !(b.parts.exponent & 0x7fff)) {
            ++b.parts.exponent;
        }
    } else {
        if ((b.parts.exponent & 0x7fff) && b.parts.mantissa == 0x8000000000000000ull) {
            --b.parts.exponent;
            b.parts.mantissa = (b.parts.exponent & 0x7fff) ? 0xffffffffffffffffull : 0x7fffffffffffffffull;
        } else {
            --b.parts.mantissa;
        }
    }
    return b.l;
}
long double nexttowardl(long double x, long double y) { return nextafterl(x, y); }
long lrintl(long double x) { return (long)rintl(x); }
long long llrintl(long double x) { return (long long)rintl(x); }
long lroundl(long double x) { return (long)roundl(x); }
long long llroundl(long double x) { return (long long)roundl(x); }

/* ---- double: exact operations ---- */
double trunc(double x) {
    double_bits v = {x};
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return x;
    if (e < 0) {
        v.u &= 1ull << 63;
        return v.d;
    }
    v.u &= ~((1ull << (52 - e)) - 1);
    return v.d;
}
double floor(double x) { double t = trunc(x); return t > x ? t - 1 : t; }
double ceil(double x) { double t = trunc(x); return t < x ? t + 1 : t; }
double round(double x) {
    double t = trunc(x), d = x - t;
    if (d >= 0.5) t += 1;
    else if (d <= -0.5) t -= 1;
    return t;
}
double rint(double x) { return (double)rintl(x); }
double nearbyint(double x) { return (double)rintl(x); }
long lrint(double x) { return (long)rintl(x); }
long long llrint(double x) { return (long long)rintl(x); }
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
double fabs(double x) { return __builtin_fabs(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
double sqrt(double x) {
#ifdef __x86_64__
    __asm__("sqrtsd %1, %0" : "=x"(x) : "x"(x));
    return x;
#else
    return (double)sqrtl(x);
#endif
}
double frexp(double x, int *e) {
    double_bits v = {x};
    int exponent = (int)((v.u >> 52) & 0x7ff);
    if (!exponent) {
        if (x == 0) {
            *e = 0;
            return x;
        }
        v.d *= 0x1p64;
        exponent = (int)((v.u >> 52) & 0x7ff) - 64;
    } else if (exponent == 0x7ff) {
        *e = 0;
        return x;
    }
    *e = exponent - 1022;
    v.u = (v.u & ~(0x7ffull << 52)) | (1022ull << 52);
    return v.d;
}
double modf(double x, double *integer) {
    *integer = trunc(x);
    if (__builtin_isinf(x)) return copysign(0, x);
    return copysign(x - *integer, x);
}
double nextafter(double x, double y) {
    if (__builtin_isnan(x) || __builtin_isnan(y)) return x + y;
    if (x == y) return y;
    double_bits v = {x};
    if (x == 0) v.u = (y < 0 ? 1ull << 63 : 0) | 1;
    else if ((x < y) == (x > 0)) ++v.u;
    else --v.u;
    return v.d;
}
double nexttoward(double x, long double y) { return nextafter(x, (double)y); }
float nextafterf(float x, float y) {
    if (__builtin_isnan(x) || __builtin_isnan(y)) return x + y;
    if (x == y) return y;
    float_bits v = {x};
    if (x == 0) v.u = (y < 0 ? 1u << 31 : 0) | 1;
    else if ((x < y) == (x > 0)) ++v.u;
    else --v.u;
    return v.f;
}
float nexttowardf(float x, long double y) { return nextafterf(x, (float)y); }
float sqrtf(float x) {
#ifdef __x86_64__
    __asm__("sqrtss %1, %0" : "=x"(x) : "x"(x));
    return x;
#else
    return (float)sqrtl(x);
#endif
}

#define UNARY(name)                                                   \
    double name(double x) { return (double)name##l(x); }              \
    float name##f(float x) { return (float)name##l(x); }
#define BINARY(name)                                                  \
    double name(double x, double y) { return (double)name##l(x, y); } \
    float name##f(float x, float y) { return (float)name##l(x, y); }
UNARY(acos) UNARY(asin) UNARY(atan) UNARY(cos) UNARY(sin) UNARY(tan) UNARY(cosh) UNARY(sinh) UNARY(tanh)
UNARY(acosh) UNARY(asinh) UNARY(atanh) UNARY(exp) UNARY(exp2) UNARY(expm1) UNARY(log) UNARY(log10)
UNARY(log1p) UNARY(log2) UNARY(logb) UNARY(cbrt) UNARY(erf) UNARY(erfc) UNARY(tgamma) UNARY(exp10)
BINARY(atan2) BINARY(hypot) BINARY(pow) BINARY(fmod) BINARY(remainder) BINARY(fdim) BINARY(fmax) BINARY(fmin)

double lgamma(double x) { return (double)lgammal(x); }
float lgammaf(float x) { return (float)lgammal(x); }
double lgamma_r(double x, int *sign) {
    double r = (double)lgammal(x);
    *sign = signgam;
    return r;
}
double pow10(double x) { return exp10(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float rintf(float x) { return (float)rintl(x); }
float nearbyintf(float x) { return (float)rintl(x); }
long lrintf(float x) { return (long)rintl(x); }
long long llrintf(float x) { return (long long)rintl(x); }
long lroundf(float x) { return (long)round(x); }
long long llroundf(float x) { return (long long)round(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
double ldexp(double x, int n) { return (double)scalbnl(x, n); }
float ldexpf(float x, int n) { return (float)scalbnl(x, n); }
double scalbn(double x, int n) { return (double)scalbnl(x, n); }
float scalbnf(float x, int n) { return (float)scalbnl(x, n); }
double scalbln(double x, long n) { return (double)scalblnl(x, n); }
float scalblnf(float x, long n) { return (float)scalblnl(x, n); }
float frexpf(float x, int *e) { return (float)frexp(x, e); }
float modff(float x, float *integer) {
    double i, r = modf(x, &i);
    *integer = (float)i;
    return (float)r;
}
int ilogb(double x) { return ilogbl(x); }
int ilogbf(float x) { return ilogbl(x); }
double remquo(double x, double y, int *q) { return (double)remquol(x, y, q); }
float remquof(float x, float y, int *q) { return (float)remquol(x, y, q); }
double nan(const char *tag) { (void)tag; return __builtin_nan(""); }
float nanf(const char *tag) { (void)tag; return __builtin_nanf(""); }
double fma(double x, double y, double z) { return (double)((long double)x * y + z); }
float fmaf(float x, float y, float z) { return (float)((double)x * y + z); }
void sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int finite(double x) { return __builtin_isfinite(x); }
int finitef(float x) { return __builtin_isfinite(x); }
int isinff(float x) { return __builtin_isinf_sign(x); }
int isnanf(float x) { return __builtin_isnan(x); }
