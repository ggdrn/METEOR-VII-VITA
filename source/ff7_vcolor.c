/*
 * Native vertex color scaling: 0x68F2D6 and 0x68F0F8 multiply the colors of
 * a model's polygon set by a brightness every frame (0x68EFCE per vertex,
 * 0x6A5BA0 for paletted sets). In battles (v0.6.35 profile, magic and
 * limits) the three were ~12% of the game thread: a call into the .so,
 * five mk_address calls and a _check_event per vertex.
 *
 * The logic is ff7_vcolor_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled functions
 * (scripts/emu/test_vcolor.py: registers, x87 state, memory, the palette
 * upload calls and _check_event calls identical over random polygon sets).
 *
 * DATA_PATH "vcolor_off.txt" (or "natives_off.txt") leaves the originals.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_vcolor.h"
#include "ff7_vcolor_core.h"
#include "ff7_anim.h"
#include "ff7_so_inline.h"
#include "ff7_switch.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>

#include <so_util/so_util.h>

extern so_module so_mod;

static vcolor_env env;

static void sets_palette_native(void) { vcolor_set_run(&env, 1); }
static void sets_native(void) { vcolor_set_run(&env, 0); }
static void scale_native(void) { vcolor_scale_run(&env); }

void ff7_vcolor_patch(void) {
    if (ff7_switch_off("ff7_vcolor", DATA_PATH "vcolor_off.txt")) return;
    static const struct {
        const char * sym;
        uint32_t prologue;
        void (* native)(void);
    } fns[] = {
        { "_Z15func_0x0068f2d6v", 0xf8dfb570, sets_palette_native }, // push {r4-r6, lr}; ldr.w r4, [pc, ...]
        { "_Z15func_0x0068f0f8v", 0x4cc6b570, sets_native },         // push {r4-r6, lr}; ldr r4, [pc, ...]
        { "_Z15func_0x0068efcev", 0x4c22b538, scale_native },        // push {r3-r5, lr}; ldr r4, [pc, ...]
    };
    static const struct {
        const char * sym;
        void (** fn)(void);
    } callees[] = {
        { "_Z15func_0x00676578v", &env.f_676578 },
        { "_Z15func_0x00670196v", &env.f_670196 },
        { "_Z15func_0x00670144v", &env.f_670144 },
        { "_Z15func_0x00660f12v", &env.f_660f12 },
        { "_Z15func_0x00660f54v", &env.f_660f54 },
    };
    uintptr_t addr[3];
    for (int i = 0; i < 3; i++) {
        addr[i] = so_symbol(&so_mod, fns[i].sym);
        if (!addr[i] || *(uint32_t *) (addr[i] & ~1u) != fns[i].prologue) {
            l_warn("ff7_vcolor: unexpected code at %s, native vertex color scaling disabled", fns[i].sym);
            return;
        }
    }
    for (int i = 0; i < 5; i++) {
        uintptr_t f = so_symbol(&so_mod, callees[i].sym);
        if (!f) {
            l_warn("ff7_vcolor: %s not found, native vertex color scaling disabled", callees[i].sym);
            return;
        }
        *callees[i].fn = (void (*)(void)) f;
    }
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    env.check_event = (void (*)(void)) so_symbol(&so_mod, "_Z12_check_eventv");
    env.regs = ff7_x86_regs();
    if (!env.mk_address || !env.check_event || !env.regs) {
        l_warn("ff7_vcolor: unexpected .so layout, native vertex color scaling disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event, &env.memmap,
                              &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_vcolor: mk_address/_check_event not as expected, calling them (slower)");
    }
    for (int i = 0; i < 3; i++) hook_addr(addr[i], (uintptr_t) fns[i].native);
    l_info("ff7_vcolor: native vertex color scaling (0x68F2D6, 0x68F0F8, 0x68EFCE) installed");
}

void ff7_vcolor_report(double seconds) {
    if (seconds <= 0 || !(env.counters[0] | env.counters[1])) return;
    l_info("vertex colors/s: %.0f sets, %.0f colors, %.0f paletted sets (native)", env.counters[0] / seconds,
           env.counters[1] / seconds, env.counters[2] / seconds);
    env.counters[0] = env.counters[1] = env.counters[2] = 0;
}
