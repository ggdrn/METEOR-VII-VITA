/* Integer implementation of the Android runtime conversions and x86 _ftol.
 * Tested against libjni_ff7.so, including its out-of-range behavior (which
 * is deliberately not replaced with C's undefined double-to-int64 cast).
 */
#ifndef FF7_FTOL_CORE_H
#define FF7_FTOL_CORE_H

#include <stdint.h>
#include <string.h>

static inline uint64_t ff7_d2ulz_bits(uint64_t bits) {
    uint32_t exp = (uint32_t)((bits >> 52) & 0x7ffu);
    uint64_t frac = bits & UINT64_C(0x000fffffffffffff);
    if (bits >> 63) return 0;
    if (exp == 0x7ffu) return frac ? 0 : UINT64_MAX;
    if (exp < 1023) return 0;
    if (exp >= 1087) return UINT64_MAX;
    uint64_t mantissa = frac | UINT64_C(0x0010000000000000);
    if (exp >= 1075) return mantissa << (exp - 1075);
    return mantissa >> (1075 - exp);
}

/* __fixdfdi: unsigned conversion of the magnitude followed by unsigned
 * two's-complement negation for negative input. E.g. -infinity returns 1
 * in this particular .so; preserving that matters more than saturating.
 */
static inline uint64_t ff7_d2lz_bits(uint64_t bits) {
    uint64_t magnitude = ff7_d2ulz_bits(bits & UINT64_C(0x7fffffffffffffff));
    return (bits >> 63) ? UINT64_C(0) - magnitude : magnitude;
}

/* State immediately BEFORE the original _check_event() call. The caller
 * must run that event and then add 4 to ESP, in that order. `frame` covers
 * entry ESP-16 .. ESP-1 and must be contiguous. TOP must be in [0,7].
 * Preserve even the scratch stack writes and control-word normalization.
 */
static inline void ff7_ftol_core(volatile uint32_t *regs, uint8_t *fpu, uint8_t *frame) {
    uint32_t top, precision, rounding;
    uint64_t bits;
    memcpy(&top, fpu + 0x60, 4);
    memcpy(&bits, fpu + top * 8, 8);
    memcpy(&precision, fpu + 0x48, 4);
    memcpy(&rounding, fpu + 0x4c, 4);
    uint16_t cw = (uint16_t)(fpu[0x40] | ((uint32_t)fpu[0x41] << 1)
        | ((uint32_t)fpu[0x42] << 2) | ((uint32_t)fpu[0x43] << 3)
        | ((uint32_t)fpu[0x44] << 4) | ((uint32_t)fpu[0x45] << 5)
        | (precision << 8) | (rounding << 10) | ((uint32_t)fpu[0x50] << 12));
    uint16_t trunc_cw = cw | 0x0c00u;
    uint32_t ebp = regs[5];
    uint64_t result = ff7_d2lz_bits(bits);
    memcpy(frame, &result, 8);
    memcpy(frame + 8, &trunc_cw, 2);
    memcpy(frame + 10, &cw, 2);
    memcpy(frame + 12, &ebp, 4);
    for (unsigned i = 0; i < 6; i++) fpu[0x40 + i] = (cw >> i) & 1;
    precision = (cw >> 8) & 3;
    rounding = (cw >> 10) & 3;
    memcpy(fpu + 0x48, &precision, 4);
    memcpy(fpu + 0x4c, &rounding, 4);
    fpu[0x50] = (cw >> 12) & 1;
    top = (top + 1) & 7;
    memcpy(fpu + 0x60, &top, 4);
    regs[0] = (uint32_t)result;
    regs[2] = (uint32_t)(result >> 32);
}

#endif
