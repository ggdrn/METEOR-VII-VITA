/*
 * source/ff7_snowboard_core.h as a freestanding ARM blob, run under emulation
 * by test_snowboard.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_snowboard_core.h"

// mode 1: 0x7334DA, 2: 0x73488D, 3: 0x734CAB
__attribute__((section(".text.entry"), used)) int entry(snowboard_env * e, int mode) {
    return mode == 1 ? snowboard_xform_run(e) : mode == 2 ? snowboard_tri_run(e) : snowboard_tri3_run(e);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
