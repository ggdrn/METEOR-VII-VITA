#include "ff7_ftol_core.h"

__attribute__((used)) void entry(volatile uint32_t *regs, uint8_t *fpu, uint8_t *frame,
                                void (*check_event)(void)) {
    ff7_ftol_core(regs, fpu, frame);
    check_event();
    regs[4] += 4;
}

__attribute__((used)) uint64_t unsigned_entry(uint64_t bits) { return ff7_d2ulz_bits(bits); }
__attribute__((used)) uint64_t signed_entry(uint64_t bits) { return ff7_d2lz_bits(bits); }

void *memcpy(void *dst, const void *src, unsigned n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
