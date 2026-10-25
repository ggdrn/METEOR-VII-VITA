/*
 * Fast, bit-exact replacement for libgcc's __fixunsdfdi (__aeabi_d2ulz).
 *
 * The ARM softfp helper builds a uint64 result through double multiply,
 * double subtract and two double-to-u32 conversions. The highway profile
 * shows the helper chain (__fixunsdfdi, __fixunsdfsi, __floatunsidf,
 * __muldf3) taking a material part of the game thread. Decode the IEEE-754
 * input directly instead. The conversion truncates toward zero; negative
 * values and NaNs produce zero, positive infinity and values >= 2^64 produce
 * UINT64_MAX, matching the existing helper's behavior.
 *
 * The first calls can be checked against the original with selftest_on.txt;
 * on a mismatch the hook is removed (ff7_unhook), so later calls run the
 * original directly instead of rewriting its entry on every call. The helper
 * is libgcc code inside the .so, so threads other than the game thread may
 * call it: the self-test (SO_CONTINUE rewrites the entry) is only for
 * diagnostics, off unless selftest_on.txt exists.
 */

#include "ff7_d2ulz.h"
#include "ff7_ftol_core.h"
#include "ff7_selftest.h"
#include "ff7_unhook.h"
#include "utils/profiler.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_D2ULZ "__fixunsdfdi"
#define D2ULZ_PROLOGUE 0xe92d40d0u /* push {r4,r6,r7,lr}; ARM state */
#define SELFTEST_CALLS 512
#define OFF_SWITCH DATA_PATH "d2ulz_off.txt"

typedef uint64_t (*d2ulz_fn)(double);

static so_hook hook;
static int selftest_left;
static int disabled;
static unsigned long long calls;

// SO_CONTINUE rewrites the entry: keep the profiler from changing the
// text's permissions meanwhile
static uint64_t original(double value) {
    profiler_hold();
    uint64_t r = SO_CONTINUE(uint64_t, hook, value);
    profiler_release();
    return r;
}

static uint64_t d2ulz_native(double value) {
    uint64_t bits, result;
    memcpy(&bits, &value, sizeof(bits));

    if (disabled) return original(value); // a call already on its way when the hook was removed

    result = ff7_d2ulz_bits(bits);
    if (selftest_left > 0) {
        uint64_t original_result;
        selftest_left--;
        original_result = original(value);
        if (original_result != result) {
            l_error("ff7_d2ulz mismatch: bits %08x%08x native %08x%08x original %08x%08x; native disabled",
                    (uint32_t)(bits >> 32), (uint32_t)bits,
                    (uint32_t)(result >> 32), (uint32_t)result,
                    (uint32_t)(original_result >> 32), (uint32_t)original_result);
            disabled = 1;
            ff7_unhook(&hook);
            return original_result;
        }
        if (!selftest_left) l_info("ff7_d2ulz selftest ok: %d conversions", SELFTEST_CALLS);
    }
    calls++;
    return result;
}

static int check_boundaries(d2ulz_fn orig_fn) {
    static const uint64_t cases[] = {
        UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
        UINT64_C(0x0000000000000001), UINT64_C(0x3fefffffffffffff),
        UINT64_C(0x3ff8000000000000), UINT64_C(0xbff0000000000000),
        UINT64_C(0x4330000000000000), UINT64_C(0x43e0000000000000),
        UINT64_C(0x43efffffffffffff), UINT64_C(0x43f0000000000000),
        UINT64_C(0x4400000000000000), UINT64_C(0x7fefffffffffffff),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000001), UINT64_C(0xfff8000000000001),
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double value;
        memcpy(&value, &cases[i], sizeof(value));
        uint64_t expected = orig_fn(value);
        uint64_t actual = ff7_d2ulz_bits(cases[i]);
        if (actual != expected) {
            l_warn("ff7_d2ulz: boundary check %u differs (native %08x%08x, original %08x%08x); helper left untouched",
                   i, (uint32_t)(actual >> 32), (uint32_t)actual,
                   (uint32_t)(expected >> 32), (uint32_t)expected);
            return 0;
        }
    }
    return 1;
}

void ff7_d2ulz_patch(void) {
    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_d2ulz: native conversion disabled by %s", OFF_SWITCH);
        return;
    }
    uintptr_t fn = so_symbol(&so_mod, SYM_D2ULZ);
    if (!fn || *(const uint32_t *)(fn & ~(uintptr_t)1) != D2ULZ_PROLOGUE) {
        l_warn("ff7_d2ulz: expected __fixunsdfdi helper not found; native conversion disabled");
        return;
    }
    if (!check_boundaries((d2ulz_fn)fn)) return;
    selftest_left = ff7_selftest_count(SELFTEST_CALLS);
    hook = hook_addr(fn, (uintptr_t)&d2ulz_native);
    l_info("ff7_d2ulz: native unsigned double-to-64 conversion installed (selftest on %d calls)", selftest_left);
}

void ff7_d2ulz_report(double seconds) {
    if (seconds > 0 && calls) l_info("unsigned double conversions/s: %.0f native", calls / seconds);
    calls = 0;
}
