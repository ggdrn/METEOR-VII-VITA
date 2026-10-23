/*
 * Native vertex color of the highway (motorbike minigame, game mode 6).
 *
 * 0x651A6D is a per-vertex callback (stored by 0x659B40, called through a
 * function pointer for every vertex of the highway): z by the camera matrix
 * (0x651A2B), a fog factor from z (0x6519D3), then each color byte times the
 * factor, saturated at 255, through three _ftol calls. In the v0.6.0 profile
 * of the minigame this chain was ~27% of the game thread (0x651A6D 10.5%,
 * _ftol 7.4%, 0x651A2B 6.0%, 0x6519D3 3.7%), and the v0.6.2 log shows
 * ~230,000 _ftol calls/s, three per vertex.
 *
 * The math is ff7_highway_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled function, which calls
 * the recompiled helpers (scripts/emu/test_highway.py: x86 registers, x87
 * state, memory and _check_event calls identical over 20,000 vertices,
 * including NaN, infinities, far == near and out-of-range control words).
 *
 * With DATA_PATH "selftest_on.txt" the first calls also run the original and
 * compare; on a mismatch the hook is removed. DATA_PATH "highway_off.txt"
 * leaves the original.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_highway.h"
#include "ff7_highway_core.h"
#include "ff7_anim.h"
#include "ff7_selftest.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/profiler.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH      DATA_PATH "highway_off.txt"
#define SYM_COLOR       "_Z15func_0x00651a6dv"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define FPU_GOT         0xf4c82c // GOT slot: pointer to the x87 state
#define COLOR_PROLOGUE  0x41f0e92d // stmdb sp!, {r4-r8, lr}
#define SELFTEST_CALLS  2000

static so_hook color_hook;
static highway_env env;
static int selftest_left, disabled;
static unsigned long long fallbacks;

static void original(void) {
    profiler_hold();
    SO_CONTINUE(int, color_hook);
    profiler_release();
}

static void color_native(void) {
    if (disabled) { // a call already on its way when the hook was removed
        original();
        return;
    }
    if (selftest_left > 0) {
        // Native on copies, then the original for real: compare
        volatile uint32_t * regs = env.regs;
        uint32_t r0[8], rn[8];
        uint8_t f0[0x68], fn[0x68];
        for (int i = 0; i < 8; i++) r0[i] = regs[i];
        memcpy(f0, env.fpu, sizeof(f0));
        uint32_t out = 0;
        memcpy(&out, env.mk_address(r0[4] + 12), 4);
        uint8_t before[3], native[3], orig[3];
        for (int i = 0; i < 3; i++) before[i] = *env.mk_address(out + i);
        if (!highway_color_run(&env)) {
            fallbacks++;
            original();
            return;
        }
        for (int i = 0; i < 8; i++) rn[i] = regs[i];
        memcpy(fn, env.fpu, sizeof(fn));
        for (int i = 0; i < 3; i++) native[i] = *env.mk_address(out + i);
        for (int i = 0; i < 8; i++) regs[i] = r0[i];
        memcpy(env.fpu, f0, sizeof(f0));
        for (int i = 0; i < 3; i++) *env.mk_address(out + i) = before[i];
        original();
        for (int i = 0; i < 3; i++) orig[i] = *env.mk_address(out + i);
        int ok = !memcmp(native, orig, 3) && !memcmp(fn + 0x40, env.fpu + 0x40, 0x68 - 0x40);
        for (int i = 0; i < 8; i++) if (rn[i] != regs[i]) ok = 0;
        selftest_left--;
        if (!ok) {
            l_error("ff7_highway selftest MISMATCH: out %02x%02x%02x/%02x%02x%02x eax %08x/%08x ecx %08x/%08x"
                    " edx %08x/%08x - native vertex color removed", native[0], native[1], native[2], orig[0],
                    orig[1], orig[2], rn[0], regs[0], rn[1], regs[1], rn[2], regs[2]);
            disabled = 1;
            ff7_unhook(&color_hook);
        } else if (!selftest_left) {
            l_info("ff7_highway selftest ok: %d vertices identical", SELFTEST_CALLS);
        }
        return;
    }
    if (!highway_color_run(&env)) {
        fallbacks++;
        original();
    }
}

void ff7_highway_patch(void) {
    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_highway: disabled by %s", OFF_SWITCH);
        return;
    }
    uintptr_t fn = so_symbol(&so_mod, SYM_COLOR);
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, SYM_MK_ADDRESS);
    env.check_event = (void (*)(void)) so_symbol(&so_mod, SYM_CHECK_EVENT);
    env.regs = ff7_x86_regs();
    env.fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    if (!fn || *(uint32_t *) (fn & ~1) != COLOR_PROLOGUE || !env.mk_address || !env.check_event || !env.regs
        || (uintptr_t) env.fpu != so_mod.text_base + 0x1492bc0u) {
        l_warn("ff7_highway: unexpected .so layout, native vertex color disabled");
        return;
    }
    selftest_left = ff7_selftest_count(SELFTEST_CALLS);
    color_hook = hook_addr(fn, (uintptr_t) &color_native);
    l_info("ff7_highway: native vertex color (0x651A6D) installed (selftest on %d vertices)", selftest_left);
}

void ff7_highway_report(double seconds) {
    if (seconds <= 0 || (!env.counters[0] && !fallbacks)) return;
    l_info("highway colored vertices/s: %.0f native, %.0f original (%.0f NaN compares)", env.counters[0] / seconds,
           fallbacks / seconds, env.counters[1] / seconds);
    env.counters[0] = env.counters[1] = 0;
    fallbacks = 0;
}
