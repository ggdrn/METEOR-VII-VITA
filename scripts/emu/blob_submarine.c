/*
 * source/ff7_submarine_core.h as a freestanding ARM blob, run under emulation
 * by test_submarine.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_submarine_core.h"

// mode 1: 0x79F903, 2: 0x7A2EF8, 3: 0x79A61E
__attribute__((section(".text.entry"), used)) int entry(submarine_env * e, int mode) {
    switch (mode) {
        case 1: return submarine_sea_run(e);
        case 2: return submarine_bits_run(e);
        case 3: return submarine_splash_run(e);
        case 5: return submarine_model_run(e);
        default: return submarine_surface_run(e);
    }
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
