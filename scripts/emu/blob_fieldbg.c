/*
 * source/ff7_fieldbg_core.h as a freestanding ARM blob, run under emulation
 * by test_fieldbg.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_fieldbg_core.h"

// mode 1: 0x64085C, 2: 0x6464BA
__attribute__((section(".text.entry"), used)) int entry(fieldbg_env * e, int mode) {
    return mode == 1 ? fieldbg_layer_run(e) : fieldbg_append_run(e);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
