/*
 * Host test for ee_softdouble.c: every routine against the host's IEEE
 * hardware (round to nearest even, subnormals enabled), on edge values and
 * millions of random operands of several distributions.
 *
 *   cc -O2 -DEE_SOFTDOUBLE_PREFIX port/ps2/rt/ee_softdouble.c \
 *      port/ps2/rt/ee_softdouble_test.c -o /tmp/ee_softdouble_test
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double ee___adddf3(double, double);
double ee___subdf3(double, double);
double ee___muldf3(double, double);
double ee___divdf3(double, double);
double ee___negdf2(double);
int ee___eqdf2(double, double);
int ee___nedf2(double, double);
int ee___ltdf2(double, double);
int ee___ledf2(double, double);
int ee___gtdf2(double, double);
int ee___gedf2(double, double);
int ee___unorddf2(double, double);
double ee___floatsidf(int32_t);
double ee___floatunsidf(uint32_t);
int32_t ee___fixdfsi(double);
uint32_t ee___fixunsdfsi(double);
double ee___extendsfdf2(float);
float ee___truncdfsf2(double);

static uint64_t s_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd64(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static double from_bits(uint64_t u)
{
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static uint64_t bits(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float ffrom(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static long s_fail;

static int same(double a, double b)
{
    if (isnan(a) || isnan(b)) {
        return isnan(a) && isnan(b);
    }
    return bits(a) == bits(b);
}

static void check2(const char *op, double a, double b, double got, double want)
{
    if (!same(got, want)) {
        if (s_fail++ < 20) {
            printf("FAIL %s %016llx %016llx -> %016llx want %016llx\n", op,
                (unsigned long long)bits(a), (unsigned long long)bits(b),
                (unsigned long long)bits(got), (unsigned long long)bits(want));
        }
    }
}

static void check_cmp(double a, double b)
{
    const int un = isnan(a) || isnan(b);
    int ok = 1;
    ok &= (ee___eqdf2(a, b) == 0) == (a == b);
    ok &= (ee___nedf2(a, b) != 0) == (a != b);
    ok &= (ee___ltdf2(a, b) < 0) == (a < b);
    ok &= (ee___ledf2(a, b) <= 0) == (a <= b);
    ok &= (ee___gtdf2(a, b) > 0) == (a > b);
    ok &= (ee___gedf2(a, b) >= 0) == (a >= b);
    ok &= (ee___unorddf2(a, b) != 0) == un;
    if (!ok && s_fail++ < 20) {
        printf("FAIL cmp %016llx %016llx\n", (unsigned long long)bits(a),
            (unsigned long long)bits(b));
    }
}

static void test_pair(double a, double b)
{
    check2("add", a, b, ee___adddf3(a, b), a + b);
    check2("sub", a, b, ee___subdf3(a, b), a - b);
    check2("mul", a, b, ee___muldf3(a, b), a * b);
    check2("div", a, b, ee___divdf3(a, b), a / b);
    check_cmp(a, b);
}

static void test_one(double a)
{
    check2("neg", a, 0, ee___negdf2(a), -a);
    const float t = ee___truncdfsf2(a);
    const float w = (float)a;
    if (!(isnan(t) && isnan(w)) && fbits(t) != fbits(w) && s_fail++ < 20) {
        printf("FAIL trunc %016llx -> %08x want %08x\n", (unsigned long long)bits(a),
            fbits(t), fbits(w));
    }
    if (!isnan(a)) {
        int32_t want;
        if (a >= 2147483648.0) {
            want = INT32_MAX;
        } else if (a <= -2147483649.0) {
            want = INT32_MIN;
        } else {
            want = (int32_t)a;
        }
        if (ee___fixdfsi(a) != want && s_fail++ < 20) {
            printf("FAIL fixdfsi %016llx -> %d want %d\n", (unsigned long long)bits(a),
                ee___fixdfsi(a), want);
        }
        uint32_t uwant = a <= -1.0 ? 0u : a >= 4294967296.0 ? UINT32_MAX : (uint32_t)(a < 0 ? 0 : a);
        if (ee___fixunsdfsi(a) != uwant && s_fail++ < 20) {
            printf("FAIL fixunsdfsi %016llx -> %u want %u\n", (unsigned long long)bits(a),
                ee___fixunsdfsi(a), uwant);
        }
    }
}

/* Random doubles: raw bits, values near each other (cancellation), small
 * exponents (subnormals), and game-like magnitudes. */
static double rnd_double(int kind)
{
    switch (kind) {
    case 0:
        return from_bits(rnd64());
    case 1:
        return from_bits((rnd64() & 0x800fffffffffffffull) | ((rnd64() % 64u) << 52));
    case 2:
        return from_bits((rnd64() & 0x800fffffffffffffull) | ((uint64_t)(1000u + rnd64() % 48u) << 52));
    default:
        return ((double)(int32_t)rnd64()) / (double)(1u + (uint32_t)rnd64() % 4096u);
    }
}

int main(void)
{
    static const uint64_t edges[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull,
        0x8000000000000001ull, 0x000fffffffffffffull, 0x0010000000000000ull,
        0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3fefffffffffffffull,
        0x3ff0000000000001ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
        0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
        0x7ff0000000000001ull, 0x4340000000000000ull, 0x41dfffffffc00000ull,
        0xc1e0000000000000ull, 0x41e0000000000000ull, 0x3fe0000000000000ull,
        0x36a0000000000000ull, 0x47efffffe0000000ull, 0x47effffff0000000ull,
        0x3810000000000000ull, 0x380fffffffffffffull, 0x369fffffffffffffull,
    };
    const size_t ne = sizeof(edges) / sizeof(edges[0]);
    for (size_t i = 0; i < ne; ++i) {
        test_one(from_bits(edges[i]));
        for (size_t j = 0; j < ne; ++j) {
            test_pair(from_bits(edges[i]), from_bits(edges[j]));
        }
    }

    long n = 0;
    for (int kind = 0; kind < 4; ++kind) {
        for (int i = 0; i < 1500000; ++i) {
            const double a = rnd_double(kind);
            double b = rnd_double(kind);
            if (i & 1) {
                /* near a: exercises cancellation and rounding ties */
                b = from_bits(bits(a) + (rnd64() % 5u) - 2u);
                if (i & 2) {
                    b = -b;
                }
            }
            test_pair(a, b);
            test_one(a);
            n += 1;
        }
    }

    for (int i = 0; i < 2000000; ++i) {
        const uint32_t u = (uint32_t)rnd64();
        const float f = ffrom(i < 256 ? (u & 0x807fffffu) : u); /* some subnormals */
        check2("extend", f, 0, ee___extendsfdf2(f), (double)f);
        check2("floatsi", 0, 0, ee___floatsidf((int32_t)u), (double)(int32_t)u);
        check2("floatunsi", 0, 0, ee___floatunsidf(u), (double)u);
    }
    const int32_t ints[] = { 0, 1, -1, INT32_MAX, INT32_MIN, 0x01000001, -0x01000001 };
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); ++i) {
        check2("floatsi", 0, 0, ee___floatsidf(ints[i]), (double)ints[i]);
    }

    printf("ee_softdouble: %ld random pairs, %ld failures\n", n, s_fail);
    return s_fail != 0;
}
