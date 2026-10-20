/*
 * source/ff7_world_tri.h as a freestanding ARM blob, run under emulation by
 * test_world_tri.py next to the recompiled 0x75F68C.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_world_tri.h"

__attribute__((section(".text.entry"), used)) int entry(world_env * e) {
    return world_tri_run(e);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
