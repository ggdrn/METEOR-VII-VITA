/*
 * Native field background tile lists: 0x64085C (a background layer's tiles
 * inside the window, appended to the draw lists) and 0x6464BA (append one
 * tile). In the chocobo ranch area after the race (v0.6.30 profile, 15 fps),
 * the two with their mk_address calls were ~7% of the game thread, ~11% once
 * the textures went to the GPU as palettes in v0.6.31.
 *
 * The logic is ff7_fieldbg_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled functions
 * (scripts/emu/test_fieldbg.py: registers, x87 state, memory and
 * _check_event calls identical over random layers).
 *
 * DATA_PATH "fieldbg_off.txt" (or "natives_off.txt") leaves the originals.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_fieldbg.h"
#include "ff7_fieldbg_core.h"
#include "ff7_anim.h"
#include "ff7_so_inline.h"
#include "ff7_switch.h"
#include "ff7_wide.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>

#include <so_util/so_util.h>

extern so_module so_mod;

static fieldbg_env env;

static int installed;

int ff7_fieldbg_installed(void) { return installed; }

static void layer_native(void) {
    env.wide = ff7_wide_tile_extra();
    fieldbg_layer_run(&env);
}
static void append_native(void) { fieldbg_append_run(&env); }

void ff7_fieldbg_patch(void) {
    if (ff7_switch_off("ff7_fieldbg", DATA_PATH "fieldbg_off.txt")) return;
    static const struct {
        const char * sym;
        uint32_t prologue;
        void (* native)(void);
    } fns[] = {
        { "_Z15func_0x0064085cv", 0x43f0e92d, layer_native },  // stmdb sp!, {r4-r9, lr}
        { "_Z15func_0x006464bav", 0x4ff0e92d, append_native }, // stmdb sp!, {r4-r11, lr}
    };
    uintptr_t addr[2];
    for (int i = 0; i < 2; i++) {
        addr[i] = so_symbol(&so_mod, fns[i].sym);
        if (!addr[i] || *(uint32_t *) (addr[i] & ~1u) != fns[i].prologue) {
            l_warn("ff7_fieldbg: unexpected code at %s, native background tile lists disabled", fns[i].sym);
            return;
        }
    }
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    env.check_event = (void (*)(void)) so_symbol(&so_mod, "_Z12_check_eventv");
    env.regs = ff7_x86_regs();
    if (!env.mk_address || !env.check_event || !env.regs) {
        l_warn("ff7_fieldbg: unexpected .so layout, native background tile lists disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event, &env.memmap,
                              &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_fieldbg: mk_address/_check_event not as expected, calling them (slower)");
    }
    for (int i = 0; i < 2; i++) hook_addr(addr[i], (uintptr_t) fns[i].native);
    installed = 1;
    l_info("ff7_fieldbg: native background tile lists (0x64085C, 0x6464BA) installed");
}

void ff7_fieldbg_report(double seconds) {
    if (seconds <= 0 || !(env.counters[0] | env.counters[2])) return;
    l_info("field background/s: %.0f layers, %.0f tiles looked at, %.0f appended (native)", env.counters[0] / seconds,
           env.counters[1] / seconds, env.counters[2] / seconds);
    env.counters[0] = env.counters[1] = env.counters[2] = 0;
}
