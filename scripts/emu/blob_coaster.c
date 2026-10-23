/*
 * source/ff7_coaster_core.h as a freestanding ARM blob, run under emulation
 * by test_coaster.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_coaster_core.h"

// mode 1: 0x5F04D7, 2: 0x5F2639, 3: 0x5F2759, 4: 0x5F2838, 5: 0x5F2897
__attribute__((section(".text.entry"), used)) int entry(coaster_env * e, int mode) {
    switch (mode) {
        case 1: return coaster_tri_run(e);
        case 2: return coaster_project_run(e);
        case 3: return coaster_depth_run(e);
        case 4: return coaster_fade_run(e);
        default: return coaster_color_run(e);
    }
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
