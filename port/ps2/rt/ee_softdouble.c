/*
 * ee_softdouble.c - IEEE 754 double arithmetic for the R5900 (EE).
 *
 * The EE FPU is single precision only, so every `double` operation in the
 * game (Rare used doubles for collision and geometry hulls) is a libgcc
 * call. The toolchain's libgcc uses the old portable "fp-bit" code. It
 * unpacks both operands into a struct, works on that, and packs the
 * result: around 300-600 cycles per operation, in __unpack_d and __pack_d
 * as much as in the arithmetic itself.
 *
 * These routines work on the bit patterns directly with the EE's 64-bit
 * integer registers. Results are bit-exact IEEE 754 (round to nearest
 * even, subnormals, infinities, quiet NaNs). tests/ee_softdouble_test.c
 * checks them against the host's hardware doubles. They replace the
 * libgcc objects one for one (each libgcc object defines only functions
 * that are all defined here), so the linker never pulls fp-bit in.
 *
 * Built for the host with EE_SOFTDOUBLE_PREFIX to test without clashing
 * with the host's own runtime.
 */

#include <stdint.h>

#ifdef EE_SOFTDOUBLE_PREFIX
#define SD_FN(name) ee_##name
#else
#define SD_FN(name) name
#endif

typedef union {
    double d;
    uint64_t u;
} sd_bits;

typedef union {
    float f;
    uint32_t u;
} sf_bits;

#define D_SIGN 0x8000000000000000ull
#define D_INF 0x7ff0000000000000ull
#define D_FRAC 0x000fffffffffffffull
#define D_IMPLICIT 0x0010000000000000ull
#define D_QUIET 0x0008000000000000ull
#define D_QNAN 0x7ff8000000000000ull
#define D_LEAD (D_IMPLICIT << 3) /* working significands: leading bit 55 */

static inline uint64_t d_bits(double d)
{
    sd_bits b;
    b.d = d;
    return b.u;
}

static inline double d_from(uint64_t u)
{
    sd_bits b;
    b.u = u;
    return b.d;
}

#if defined(_MIPS_ARCH_R5900)
/* The R5900 has no CLZ/DCLZ (__builtin_clzll would call libgcc). PLZCW
 * counts the bits below each word's sign bit that equal it, so for a word
 * with bit 31 clear it is clz - 1 (31 for 0). */
static inline int clz32(uint32_t x)
{
    if (x & 0x80000000u) {
        return 0;
    }
    uint64_t r;
    __asm__("plzcw %0, %1" : "=r"(r) : "r"((uint64_t)x));
    return (int)(uint32_t)r + 1;
}

static inline int clz64(uint64_t x)
{
    const uint32_t hi = (uint32_t)(x >> 32);
    return hi != 0 ? clz32(hi) : 32 + clz32((uint32_t)x);
}
#else
static inline int clz64(uint64_t x)
{
    return __builtin_clzll(x);
}
#endif

/* Shift a subnormal significand up to the implicit bit; returns the
 * exponent it then has (1 - shift). */
static inline int normalize(uint64_t *sig)
{
    const int shift = clz64(*sig) - clz64(D_IMPLICIT);
    *sig <<= shift;
    return 1 - shift;
}

/*
 * Round and pack. sig has its leading bit at bit 55 (three extra bits below
 * the 52-bit fraction, the lowest of them sticky) or is 0; exp is the
 * biased exponent of that leading bit.
 */
static uint64_t round_pack(uint64_t sign, int exp, uint64_t sig)
{
    if (exp >= 0x7ff) {
        return sign | D_INF;
    }
    if (exp <= 0) {
        const int shift = 1 - exp;
        if (shift > 56) {
            sig = sig != 0;
        } else {
            const uint64_t sticky = (sig << (64 - shift)) != 0;
            sig = (sig >> shift) | sticky;
        }
        exp = 0;
    }
    const uint32_t rgs = (uint32_t)(sig & 7u);
    uint64_t result = sig >> 3;
    if (exp > 0) {
        result = (result & D_FRAC) | ((uint64_t)exp << 52);
    }
    if (rgs > 4u || (rgs == 4u && (result & 1u))) {
        ++result; /* may carry into the exponent, up to infinity */
    }
    return sign | result;
}

static uint64_t add_bits(uint64_t a, uint64_t b)
{
    uint64_t a_abs = a & ~D_SIGN;
    uint64_t b_abs = b & ~D_SIGN;

    if (a_abs - 1u >= D_INF - 1u || b_abs - 1u >= D_INF - 1u) {
        if (a_abs > D_INF) {
            return a | D_QUIET;
        }
        if (b_abs > D_INF) {
            return b | D_QUIET;
        }
        if (a_abs == D_INF) {
            return (a ^ b) == D_SIGN ? D_QNAN : a;
        }
        if (b_abs == D_INF) {
            return b;
        }
        if (a_abs == 0) {
            return b_abs == 0 ? (a & b) : b;
        }
        if (b_abs == 0) {
            return a;
        }
    }

    if (b_abs > a_abs) {
        const uint64_t t = a;
        a = b;
        b = t;
        const uint64_t ta = a_abs;
        a_abs = b_abs;
        b_abs = ta;
    }

    int a_exp = (int)(a_abs >> 52);
    int b_exp = (int)(b_abs >> 52);
    uint64_t a_sig = a_abs & D_FRAC;
    uint64_t b_sig = b_abs & D_FRAC;
    if (a_exp == 0) {
        a_exp = normalize(&a_sig);
    }
    if (b_exp == 0) {
        b_exp = normalize(&b_sig);
    }

    const uint64_t sign = a & D_SIGN;
    const int subtract = ((a ^ b) & D_SIGN) != 0;
    a_sig = (a_sig | D_IMPLICIT) << 3;
    b_sig = (b_sig | D_IMPLICIT) << 3;

    const int align = a_exp - b_exp;
    if (align != 0) {
        if (align < 64) {
            const uint64_t sticky = (b_sig << (64 - align)) != 0;
            b_sig = (b_sig >> align) | sticky;
        } else {
            b_sig = 1;
        }
    }

    if (subtract) {
        a_sig -= b_sig;
        if (a_sig == 0) {
            return 0; /* x - x is +0 in round to nearest */
        }
        if (a_sig < D_LEAD) {
            const int shift = clz64(a_sig) - clz64(D_LEAD);
            a_sig <<= shift;
            a_exp -= shift;
        }
    } else {
        a_sig += b_sig;
        if (a_sig & (D_LEAD << 1)) {
            const uint64_t sticky = a_sig & 1u;
            a_sig = (a_sig >> 1) | sticky;
            ++a_exp;
        }
    }
    return round_pack(sign, a_exp, a_sig);
}

/* 64 x 64 -> 128 with the EE's 32 x 32 -> 64 multiplier. */
static inline void mul_wide(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    const uint64_t al = (uint32_t)a, ah = a >> 32;
    const uint64_t bl = (uint32_t)b, bh = b >> 32;
    const uint64_t ll = al * bl;
    const uint64_t lh = al * bh;
    const uint64_t hl = ah * bl;
    const uint64_t hh = ah * bh;
    const uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *lo = (mid << 32) | (uint32_t)ll;
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

static uint64_t mul_bits(uint64_t a, uint64_t b)
{
    const uint64_t a_abs = a & ~D_SIGN;
    const uint64_t b_abs = b & ~D_SIGN;
    const uint64_t sign = (a ^ b) & D_SIGN;
    int a_exp = (int)(a_abs >> 52);
    int b_exp = (int)(b_abs >> 52);
    uint64_t a_sig = a_abs & D_FRAC;
    uint64_t b_sig = b_abs & D_FRAC;

    if (a_exp - 1u >= 0x7feu || b_exp - 1u >= 0x7feu) {
        if (a_abs > D_INF) {
            return a | D_QUIET;
        }
        if (b_abs > D_INF) {
            return b | D_QUIET;
        }
        if (a_abs == D_INF) {
            return b_abs != 0 ? (sign | D_INF) : D_QNAN;
        }
        if (b_abs == D_INF) {
            return a_abs != 0 ? (sign | D_INF) : D_QNAN;
        }
        if (a_abs == 0 || b_abs == 0) {
            return sign;
        }
        if (a_exp == 0) {
            a_exp = normalize(&a_sig);
        }
        if (b_exp == 0) {
            b_exp = normalize(&b_sig);
        }
    }

    a_sig |= D_IMPLICIT;
    b_sig |= D_IMPLICIT;
    uint64_t hi, lo;
    mul_wide(a_sig, b_sig, &hi, &lo); /* 105 or 106 significant bits */
    int exp = a_exp + b_exp - 1022;
    if (!(hi & (1ull << 41))) { /* leading bit at 104: bring it to 105 */
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
        --exp;
    }
    /* bits 105..50 -> 55..0, the rest sticky */
    const uint64_t sticky = (lo & ((1ull << 50) - 1u)) != 0;
    const uint64_t sig = (hi << 14) | (lo >> 50) | sticky;
    return round_pack(sign, exp, sig);
}

static uint64_t div_bits(uint64_t a, uint64_t b)
{
    const uint64_t a_abs = a & ~D_SIGN;
    const uint64_t b_abs = b & ~D_SIGN;
    const uint64_t sign = (a ^ b) & D_SIGN;
    int a_exp = (int)(a_abs >> 52);
    int b_exp = (int)(b_abs >> 52);
    uint64_t a_sig = a_abs & D_FRAC;
    uint64_t b_sig = b_abs & D_FRAC;

    if (a_exp - 1u >= 0x7feu || b_exp - 1u >= 0x7feu) {
        if (a_abs > D_INF) {
            return a | D_QUIET;
        }
        if (b_abs > D_INF) {
            return b | D_QUIET;
        }
        if (a_abs == D_INF) {
            return b_abs == D_INF ? D_QNAN : (sign | D_INF);
        }
        if (b_abs == D_INF) {
            return sign;
        }
        if (a_abs == 0) {
            return b_abs == 0 ? D_QNAN : sign;
        }
        if (b_abs == 0) {
            return sign | D_INF;
        }
        if (a_exp == 0) {
            a_exp = normalize(&a_sig);
        }
        if (b_exp == 0) {
            b_exp = normalize(&b_sig);
        }
    }

    a_sig |= D_IMPLICIT;
    b_sig |= D_IMPLICIT;
    int exp = a_exp - b_exp + 1023;
    if (a_sig < b_sig) {
        a_sig <<= 1;
        --exp;
    }
    /* 55 quotient bits: 1 integer, 54 fraction; the remainder is sticky. */
    uint64_t rem = a_sig;
    uint64_t q = 0;
    for (int i = 0; i < 55; ++i) {
        q <<= 1;
        if (rem >= b_sig) {
            rem -= b_sig;
            q |= 1u;
        }
        rem <<= 1;
    }
    return round_pack(sign, exp, (q << 1) | (rem != 0));
}

/* -1 less, 0 equal, 1 greater, 2 unordered */
static inline int cmp_bits(uint64_t a, uint64_t b)
{
    const uint64_t a_abs = a & ~D_SIGN;
    const uint64_t b_abs = b & ~D_SIGN;
    if (a_abs > D_INF || b_abs > D_INF) {
        return 2;
    }
    if ((a_abs | b_abs) == 0) {
        return 0;
    }
    const int64_t sa = (int64_t)a, sb = (int64_t)b;
    if ((sa & sb) < 0) { /* both negative: order reverses */
        return sa > sb ? -1 : sa < sb ? 1 : 0;
    }
    return sa < sb ? -1 : sa > sb ? 1 : 0;
}

double SD_FN(__adddf3)(double a, double b)
{
    return d_from(add_bits(d_bits(a), d_bits(b)));
}

double SD_FN(__subdf3)(double a, double b)
{
    return d_from(add_bits(d_bits(a), d_bits(b) ^ D_SIGN));
}

double SD_FN(__muldf3)(double a, double b)
{
    return d_from(mul_bits(d_bits(a), d_bits(b)));
}

double SD_FN(__divdf3)(double a, double b)
{
    return d_from(div_bits(d_bits(a), d_bits(b)));
}

double SD_FN(__negdf2)(double a)
{
    return d_from(d_bits(a) ^ D_SIGN);
}

/* libgcc's comparison contract: the result compared with 0 gives the
 * relation; unordered makes every relation but != false. */
int SD_FN(__eqdf2)(double a, double b)
{
    return cmp_bits(d_bits(a), d_bits(b)) != 0;
}

int SD_FN(__nedf2)(double a, double b)
{
    return cmp_bits(d_bits(a), d_bits(b)) != 0;
}

int SD_FN(__ltdf2)(double a, double b)
{
    const int c = cmp_bits(d_bits(a), d_bits(b));
    return c == 2 ? 1 : c;
}

int SD_FN(__ledf2)(double a, double b)
{
    const int c = cmp_bits(d_bits(a), d_bits(b));
    return c == 2 ? 1 : c;
}

int SD_FN(__gtdf2)(double a, double b)
{
    const int c = cmp_bits(d_bits(a), d_bits(b));
    return c == 2 ? -1 : c;
}

int SD_FN(__gedf2)(double a, double b)
{
    const int c = cmp_bits(d_bits(a), d_bits(b));
    return c == 2 ? -1 : c;
}

int SD_FN(__unorddf2)(double a, double b)
{
    return cmp_bits(d_bits(a), d_bits(b)) == 2;
}

double SD_FN(__floatsidf)(int32_t i)
{
    if (i == 0) {
        return 0.0;
    }
    const uint64_t sign = i < 0 ? D_SIGN : 0;
    uint64_t m = i < 0 ? (uint64_t)(-(int64_t)i) : (uint64_t)i;
    const int shift = clz64(m) - clz64(D_IMPLICIT);
    m <<= shift; /* exact: at most 32 significant bits */
    return d_from(sign | ((uint64_t)(1075 - shift) << 52) | (m & D_FRAC));
}

double SD_FN(__floatunsidf)(uint32_t i)
{
    if (i == 0) {
        return 0.0;
    }
    uint64_t m = i;
    const int shift = clz64(m) - clz64(D_IMPLICIT);
    m <<= shift;
    return d_from(((uint64_t)(1075 - shift) << 52) | (m & D_FRAC));
}

/* Truncates toward zero; NaN gives 0 and out-of-range values saturate, as
 * libgcc's fp-bit did. */
int32_t SD_FN(__fixdfsi)(double a)
{
    const uint64_t u = d_bits(a);
    const uint64_t abs = u & ~D_SIGN;
    const int neg = (u & D_SIGN) != 0;
    if (abs > D_INF) {
        return 0;
    }
    const int exp = (int)(abs >> 52) - 1023;
    if (exp < 0) {
        return 0;
    }
    if (exp >= 31) {
        return neg ? INT32_MIN : INT32_MAX;
    }
    const uint32_t m = (uint32_t)(((abs & D_FRAC) | D_IMPLICIT) >> (52 - exp));
    return neg ? -(int32_t)m : (int32_t)m;
}

uint32_t SD_FN(__fixunsdfsi)(double a)
{
    const uint64_t u = d_bits(a);
    const uint64_t abs = u & ~D_SIGN;
    if ((u & D_SIGN) || abs > D_INF) {
        return 0;
    }
    const int exp = (int)(abs >> 52) - 1023;
    if (exp < 0) {
        return 0;
    }
    if (exp >= 32) {
        return UINT32_MAX;
    }
    return (uint32_t)(((abs & D_FRAC) | D_IMPLICIT) >> (52 - exp));
}

double SD_FN(__extendsfdf2)(float f)
{
    sf_bits b;
    b.f = f;
    const uint64_t sign = (uint64_t)(b.u & 0x80000000u) << 32;
    const uint32_t abs = b.u & 0x7fffffffu;
    const uint32_t exp = abs >> 23;
    uint64_t frac = abs & 0x7fffffu;
    if (exp == 0xffu) {
        return d_from(sign | D_INF | (frac << 29) | (frac ? D_QUIET : 0));
    }
    if (exp == 0) {
        if (frac == 0) {
            return d_from(sign);
        }
        const int shift = clz64(frac) - clz64(0x800000ull);
        frac = (frac << shift) & 0x7fffffu;
        return d_from(sign | ((uint64_t)(1023 - 126 - shift) << 52) | (frac << 29));
    }
    return d_from(sign | ((uint64_t)(exp + 1023 - 127) << 52) | (frac << 29));
}

float SD_FN(__truncdfsf2)(double a)
{
    const uint64_t u = d_bits(a);
    const uint32_t sign = (uint32_t)(u >> 32) & 0x80000000u;
    const uint64_t abs = u & ~D_SIGN;
    sf_bits r;
    if (abs > D_INF) {
        r.u = sign | 0x7fc00000u | (uint32_t)((abs & D_FRAC) >> 29);
        return r.f;
    }
    if (abs == D_INF) {
        r.u = sign | 0x7f800000u;
        return r.f;
    }
    if (abs == 0) {
        r.u = sign;
        return r.f;
    }
    int exp = (int)(abs >> 52);
    uint64_t sig = abs & D_FRAC;
    if (exp == 0) {
        exp = normalize(&sig); /* far below float range: rounds to 0 */
    }
    sig |= D_IMPLICIT;
    /* 53 bits -> 24 + 3 (leading bit at 26), rest sticky */
    uint64_t s = (sig >> 26) | ((sig & ((1ull << 26) - 1u)) != 0);
    int fexp = exp - 1023 + 127;
    if (fexp >= 0xff) {
        r.u = sign | 0x7f800000u;
        return r.f;
    }
    if (fexp <= 0) {
        const int shift = 1 - fexp;
        if (shift > 27) {
            s = s != 0;
        } else {
            const uint64_t sticky = (s << (64 - shift)) != 0;
            s = (s >> shift) | sticky;
        }
        fexp = 0;
    }
    const uint32_t rgs = (uint32_t)(s & 7u);
    uint32_t result = (uint32_t)(s >> 3);
    if (fexp > 0) {
        result = (result & 0x7fffffu) | ((uint32_t)fexp << 23);
    }
    if (rgs > 4u || (rgs == 4u && (result & 1u))) {
        ++result;
    }
    r.u = sign | result;
    return r.f;
}
