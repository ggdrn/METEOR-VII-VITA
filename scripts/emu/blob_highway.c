/*
 * source/ff7_highway_core.h as a freestanding ARM blob, run under emulation
 * by test_highway.py next to the recompiled 0x651A6D.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_highway_core.h"

__attribute__((section(".text.entry"), used)) int entry(highway_env * e) {
    return highway_color_run(e);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
