/*
 * source/ff7_engine_core.h as a freestanding ARM blob, run under emulation
 * by test_engine.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_engine_core.h"

__attribute__((section(".text.entry"), used)) int entry(engine_env * e, int which) {
    return which == 0 ? engine_matrix_run(e) : which == 1 ? engine_fixdiv_run(e)
         : which == 2 ? engine_vector_run(e) : engine_alloc_run(e);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
