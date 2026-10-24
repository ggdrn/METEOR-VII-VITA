/*
 * source/ff7_vcolor_core.h as a freestanding ARM blob, run under emulation
 * by test_vcolor.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_vcolor_core.h"

// mode 1: 0x68F2D6, 2: 0x68F0F8, 3: 0x68EFCE
__attribute__((section(".text.entry"), used)) int entry(vcolor_env * e, int mode) {
    return mode == 3 ? vcolor_scale_run(e) : vcolor_set_run(e, mode == 1);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
