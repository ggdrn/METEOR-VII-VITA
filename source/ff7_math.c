/*
 * Native cos/sin for libjni_ff7.so.
 *
 * ff7.exe calls the MSVC runtime's _CIcos (0x7AFE00) and _CIsin (0x7AFEB0)
 * with the argument in x87 st(0). Recompiled, each call runs ~1.3 KB of
 * emulated control-word bookkeeping around a call to libm cos()/sin():
 * profiling showed them at ~5% of the game thread in busy field scenes.
 *
 * The recompiler keeps the x87 stack as doubles in a global struct
 * (st(i) = regs[top + i], top at +0x60). The replacement computes
 * st(0) = cos(st(0)) with the same libm function the recompiled code calls,
 * so results are identical; arguments outside fcos/fsin's range (|x| >= 2^63)
 * and non-finite values still go through the original.
 *
 * Both functions are cdecl helpers that leave EAX/ECX/EDX undefined; the
 * net stack effect is the return (ESP += 4).
 *
 * The first calls are checked against the original.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#pragma GCC optimize ("no-fast-math")

#include "ff7_math.h"
#include "ff7_switch.h"
#include "ff7_anim.h"
#include "ff7_selftest.h"

#include "utils/logger.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_COS         "_Z15func_0x007afe00v"
#define SYM_SIN         "_Z15func_0x007afeb0v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define FPU_GOT         0xf4c82c // GOT slot: pointer to the x87 state
#define FPU_TOP         0x60
#define SELFTEST_CALLS  500

enum { ESP = 4 };

typedef void (* check_event_fn)(void);

static check_event_fn check_event;
static volatile uint32_t * regs;
static uint8_t * fpu;
static so_hook cos_hook, sin_hook;
static int selftest_cos = SELFTEST_CALLS, selftest_sin = SELFTEST_CALLS;
static int disabled_cos, disabled_sin;
static unsigned long long calls;

static inline double * st0(void) {
    uint32_t top = *(uint32_t *) (fpu + FPU_TOP);
    return (double *) (fpu + top * 8);
}

static inline int in_range(double x) {
    return isfinite(x) && fabs(x) < 9.2233720368547758e18; // 2^63
}

#define NATIVE_TRIG(name, fn, hook, selftest, disabled)                                  \
    static void name(void) {                                                             \
        double * s = st0();                                                              \
        double x = *s;                                                                   \
        if (disabled || !in_range(x)) {                                                  \
            SO_CONTINUE(int, hook);                                                      \
            return;                                                                      \
        }                                                                                \
        double r = fn(x);                                                                \
        if (selftest > 0) {                                                              \
            selftest--;                                                                  \
            SO_CONTINUE(int, hook);                                                      \
            double o = *st0();                                                           \
            if (memcmp(&o, &r, sizeof(r))) {                                             \
                l_error("ff7_math " #fn " selftest MISMATCH: x %a native %a original %a" \
                        " - native " #fn " disabled", x, r, o);                          \
                disabled = 1;                                                            \
            } else if (selftest == 0) {                                                  \
                l_info("ff7_math " #fn " selftest ok: %d calls identical", SELFTEST_CALLS); \
            }                                                                            \
            return;                                                                      \
        }                                                                                \
        *s = r;                                                                          \
        check_event();                                                                   \
        regs[ESP] += 4; /* ret */                                                        \
        calls++;                                                                         \
    }

NATIVE_TRIG(cos_native, cos, cos_hook, selftest_cos, disabled_cos)
NATIVE_TRIG(sin_native, sin, sin_hook, selftest_sin, disabled_sin)

void ff7_math_patch(void) {
    if (ff7_switch_off("ff7_math", DATA_PATH "math_off.txt")) return;
    uintptr_t c = so_symbol(&so_mod, SYM_COS), s = so_symbol(&so_mod, SYM_SIN);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    regs = ff7_x86_regs();
    fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    uintptr_t f = (uintptr_t) fpu;

    if (!c || !s || !check_event || !regs || f < so_mod.text_base || f >= so_mod.text_base + 0x1498e40) {
        l_warn("ff7_math: unexpected .so layout, native cos/sin disabled");
        return;
    }
    selftest_cos = selftest_sin = ff7_selftest_count(SELFTEST_CALLS);
    cos_hook = hook_addr(c, (uintptr_t) &cos_native);
    sin_hook = hook_addr(s, (uintptr_t) &sin_native);
    l_info("ff7_math: native cos/sin installed (x87 state at %p, selftest on %d calls)", fpu, selftest_cos);
}

void ff7_math_report(double seconds) {
    if (seconds <= 0 || !calls) return;
    l_info("cos/sin calls/s: %.0f (native)", calls / seconds);
    calls = 0;
}
