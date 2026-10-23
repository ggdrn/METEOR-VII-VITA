/*
 * source/ff7_chocobo_core.h as a freestanding ARM blob, run under emulation
 * by test_chocobo.py next to the recompiled functions.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")
#include "ff7_chocobo_core.h"

// mode 3/4: 0x77B9D4/0x77BCA4, 13/14: 0x77BB50/0x77BE95, 23/24: 0x770767/0x7706D7, 30: 0x770305
__attribute__((section(".text.entry"), used)) int entry(chocobo_env * e, int mode) {
    if (mode < 10) return chocobo_prim_run(e, mode);
    if (mode < 20) return chocobo_wrap_run(e, mode - 10);
    if (mode < 30) return chocobo_obj_run(e, mode - 23);
    return chocobo_list_run(e);
}

void * memcpy(void * d, const void * s, unsigned n) {
    unsigned char * dd = d;
    const unsigned char * ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
