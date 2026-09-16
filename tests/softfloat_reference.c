/* System-compiled oracle. No part of this file supplies arithmetic to 12c. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern double sf_mul(double, double);
extern double sf_div(double, double);
extern double sf_unsigned(unsigned long);
extern double sf_signed(long);
extern double sf_int(int);
extern double sf_uint(unsigned);

static uint64_t bits(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}
static double value(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}
static int equal(double actual, double expected, const char *op, uint64_t a, uint64_t b) {
    uint64_t x=bits(actual), y=bits(expected);
    uint64_t magnitude=UINT64_C(0x7fffffffffffffff);
    uint64_t infinity=UINT64_C(0x7ff0000000000000);
    if (x == y || ((x & magnitude) > infinity && (y & magnitude) > infinity)) return 1;
    fprintf(stderr, "%s(%016llx,%016llx): got %016llx expected %016llx\n",
            op, (unsigned long long)a, (unsigned long long)b,
            (unsigned long long)x, (unsigned long long)y);
    return 0;
}
static int pair(uint64_t a, uint64_t b) {
    volatile double x=value(a), y=value(b);
    return equal(sf_mul(x,y), x*y, "mul", a,b) &&
           equal(sf_div(x,y), x/y, "div", a,b);
}
static uint64_t random_bits(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}
int sf_check(void) {
    static const uint64_t edge[] = {
        0, UINT64_C(0x8000000000000000), 1, 2, 3,
        UINT64_C(0x8000000000000001), UINT64_C(0x000fffffffffffff),
        UINT64_C(0x0010000000000000), UINT64_C(0x0010000000000001),
        UINT64_C(0x3fdfffffffffffff), UINT64_C(0x3fe0000000000000),
        UINT64_C(0x3fe0000000000001), UINT64_C(0x3fefffffffffffff),
        UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000001),
        UINT64_C(0x3ff8000000000000), UINT64_C(0x4000000000000000),
        UINT64_C(0xbff0000000000000), UINT64_C(0x4340000000000000),
        UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000001)
    };
    for (unsigned i=0;i<sizeof(edge)/sizeof(*edge);++i)
        for (unsigned j=0;j<sizeof(edge)/sizeof(*edge);++j)
            if (!pair(edge[i],edge[j])) return 1;

    const long signed_edge[] = {LONG_MIN, LONG_MIN+1, -9007199254740995L,
        -9007199254740993L, -1, 0, 1, 9007199254740991L, 9007199254740992L,
        9007199254740993L, 9007199254740995L, LONG_MAX};
    for (unsigned i=0;i<sizeof(signed_edge)/sizeof(*signed_edge);++i) {
        volatile long n=signed_edge[i];
        if (!equal(sf_signed(n),(double)n,"signed",(uint64_t)n,0)) return 2;
    }
    const unsigned long unsigned_edge[] = {0, 1, 9007199254740991UL,
        9007199254740992UL, 9007199254740993UL, 9007199254740995UL,
        9223372036854775808UL, ULONG_MAX-1024, ULONG_MAX};
    for (unsigned i=0;i<sizeof(unsigned_edge)/sizeof(*unsigned_edge);++i) {
        volatile unsigned long n=unsigned_edge[i];
        if (!equal(sf_unsigned(n),(double)n,"unsigned",n,0)) return 3;
    }
    if (!equal(sf_int(INT_MIN),(double)INT_MIN,"int",INT_MIN,0) ||
        !equal(sf_uint(UINT_MAX),(double)UINT_MAX,"uint",UINT_MAX,0)) return 4;

    uint64_t state=UINT64_C(0x123456789abcdef);
    for (unsigned i=0;i<2048;++i) {
        uint64_t a=random_bits(&state), b=random_bits(&state);
        if (!pair(a,b)) return 5;
        volatile unsigned long u=a;
        long signed_value;
        memcpy(&signed_value,&a,8);
        volatile long s=signed_value;
        if (!equal(sf_unsigned(u),(double)u,"unsigned",u,0) ||
            !equal(sf_signed(s),(double)s,"signed",a,0)) return 6;
    }
    return 0;
}
