/* This source is compiled by 12c, never by the host compiler. SF preserves a
   readable C implementation while embedding it in the compiler executable.
   All arithmetic routines therefore undergo the ordinary instruction audit.
   Values and results are binary64 bit patterns, passed in integer registers. */
#define SF(...) #__VA_ARGS__ "\n",
static const char *const softfloat_source[] = {
SF(unsigned long __12c_sf_shr1(unsigned long value);)
SF(unsigned long __12c_sf_shr(unsigned long value, int count) {
    if (count >= 64) return 0UL;
    while (count > 0) { value=__12c_sf_shr1(value); count-=1; }
    return value;
})
SF(unsigned long __12c_sf_jam(unsigned long value, int count) {
    if (count >= 64) { if (value) return 1UL; return 0UL; }
    while (count > 0) {
        unsigned long next=__12c_sf_shr1(value);
        if (value != next+next) {
            unsigned long half=__12c_sf_shr1(next);
            if (next == half+half) next+=1;
        }
        value=next;
        count-=1;
    }
    return value;
})
/* ext represents value * 2^(55-exponent). Normalize to bit 55 and round
   using three low bits (guard, round, sticky), nearest with ties to even. */
SF(unsigned long __12c_sf_pack(unsigned long sign, int exponent, unsigned long ext) {
    if (!ext) return sign;
    while (ext >= 72057594037927936UL) {
        ext=__12c_sf_jam(ext,1); exponent+=1;
    }
    while (ext < 36028797018963968UL) { ext=ext+ext; exponent-=1; }
    if (exponent < -1022) {
        ext=__12c_sf_jam(ext,-1022-exponent); exponent=-1022;
    }
    unsigned long rounded=__12c_sf_shr(ext,3);
    unsigned long tail=ext-rounded*8UL;
    unsigned long half=__12c_sf_shr1(rounded);
    if (tail > 4UL || (tail == 4UL && rounded != half+half)) rounded+=1;
    if (rounded >= 9007199254740992UL) {
        rounded=__12c_sf_shr1(rounded); exponent+=1;
    }
    if (exponent > 1023) return sign+9218868437227405312UL;
    if (rounded < 4503599627370496UL) return sign+rounded;
    return sign+(unsigned long)(exponent+1023)*4503599627370496UL
               +rounded-4503599627370496UL;
})
SF(unsigned long __12c_sf_u64(unsigned long value) {
    return __12c_sf_pack(0UL,55,value);
})
SF(unsigned long __12c_sf_i64(long value) {
    unsigned long sign=0UL;
    unsigned long magnitude=(unsigned long)value;
    if (value < 0L) { sign=9223372036854775808UL; magnitude=0UL-magnitude; }
    return __12c_sf_pack(sign,55,magnitude);
})
/* op=0 multiply, op=1 divide. Decode into normalized 53-bit significands;
   all intermediate limbs and remainders fit unsigned long. */
SF(unsigned long __12c_sf_binary(unsigned long a, unsigned long b, int op) {
    unsigned long sign=0UL;
    if (a >= 9223372036854775808UL) { a-=9223372036854775808UL; sign=9223372036854775808UL; }
    if (b >= 9223372036854775808UL) { b-=9223372036854775808UL; sign=9223372036854775808UL-sign; }
    if (a > 9218868437227405312UL || b > 9218868437227405312UL)
        return 9221120237041090560UL;
    if (!op) {
        if ((a == 9218868437227405312UL && !b) ||
            (b == 9218868437227405312UL && !a)) return 9221120237041090560UL;
        if (a == 9218868437227405312UL || b == 9218868437227405312UL)
            return sign+9218868437227405312UL;
        if (!a || !b) return sign;
    } else {
        if ((!a && !b) || (a == 9218868437227405312UL && b == 9218868437227405312UL))
            return 9221120237041090560UL;
        if (a == 9218868437227405312UL || !b) return sign+9218868437227405312UL;
        if (!a || b == 9218868437227405312UL) return sign;
    }
    int ea=(int)__12c_sf_shr(a,52);
    int eb=(int)__12c_sf_shr(b,52);
    unsigned long ma=a-(unsigned long)ea*4503599627370496UL;
    unsigned long mb=b-(unsigned long)eb*4503599627370496UL;
    if (ea) ma+=4503599627370496UL; else ea=1;
    if (eb) mb+=4503599627370496UL; else eb=1;
    ea-=1023; eb-=1023;
    while (ma < 4503599627370496UL) { ma=ma+ma; ea-=1; }
    while (mb < 4503599627370496UL) { mb=mb+mb; eb-=1; }
    if (op) {
        if (ma < mb) { ma=ma+ma; ea-=1; }
        unsigned long quotient=0UL;
        int i=0;
        while (i < 56) {
            quotient=quotient+quotient;
            if (ma >= mb) { ma-=mb; quotient+=1; }
            ma=ma+ma;
            i+=1;
        }
        unsigned long half=__12c_sf_shr1(quotient);
        if (ma && quotient == half+half) quotient+=1;
        return __12c_sf_pack(sign,ea-eb,quotient);
    }
    unsigned long hi=0UL, lo=0UL, ahi=0UL;
    int i=0;
    while (i < 53) {
        unsigned long half=__12c_sf_shr1(mb);
        if (mb != half+half) {
            unsigned long next=lo+ma;
            hi+=ahi;
            if (next < lo) hi+=1;
            lo=next;
        }
        ahi=ahi+ahi;
        if (ma >= 9223372036854775808UL) ahi+=1;
        ma=ma+ma;
        mb=half;
        i+=1;
    }
    unsigned long lowtop=__12c_sf_shr(lo,49);
    unsigned long ext=hi*32768UL+lowtop;
    unsigned long half=__12c_sf_shr1(ext);
    if (lo != lowtop*562949953421312UL && ext == half+half) ext+=1;
    return __12c_sf_pack(sign,ea+eb,ext);
})
};
#undef SF
