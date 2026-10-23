/*
 * Native submarine sea update, leading-bit count, splashes, sea surface and
 * models (the submarines, mines, ...: 0x79FB20). Shared core checked against
 * libjni_ff7.so by scripts/emu/test_submarine.py. The submarine has 30 Hz
 * logic; its frame limiter 0x78C9E1 is replaced in ff7_framecap.c.
 * DATA_PATH "submarine_off.txt" or "natives_off.txt" leaves the originals.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_submarine.h"
#include "ff7_submarine_core.h"
#include "ff7_anim.h"
#include "ff7_so_inline.h"
#include "ff7_switch.h"
#include "ff7_unhook.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;
#define FPU_GOT 0xf4c82c

static submarine_env env;
static int installed, enabled;
enum { SEA, BITS, SPLASH, SURFACE, MODEL, NUM_F };
static uint64_t surface_us;
static unsigned surface_fallbacks;
static const struct {
    const char * sym;
    uint32_t prologue;
} fns[NUM_F] = {
    { "_Z15func_0x0079f903v", 0x4df0e92d },
    { "_Z15func_0x007a2ef8v", 0x2500b538 },
    { "_Z15func_0x0079a61ev", 0x4ff0e92d },
    { "_Z15func_0x007a0190v", 0x43f0e92d },
    { "_Z15func_0x0079fb20v", 0x4ff0e92d },
};
// 0x79FB20's matrix set-up, called as x86 functions (env.f_model)
static const char * const model_syms[7] = {
    "_Z15func_0x006625abv", "_Z15func_0x006627cdv", "_Z15func_0x006626bcv", "_Z15func_0x00663673v",
    "_Z15func_0x00662035v", "_Z15func_0x00663707v", "_Z15func_0x00662eccv",
};
static uintptr_t addrs[NUM_F];
static so_hook hooks[NUM_F];

static void sea_native(void) { submarine_sea_run(&env); }
static void bits_native(void) { submarine_bits_run(&env); }
static void splash_native(void) { submarine_splash_run(&env); }
static void model_native(void) { submarine_model_run(&env); }
static void surface_native(void) {
    uint64_t start = sceKernelGetProcessTimeWide();
    if (!submarine_surface_run(&env)) {
        surface_fallbacks++;
        SO_CONTINUE_HELD(int, hooks[SURFACE]);
    }
    surface_us += sceKernelGetProcessTimeWide() - start;
}

void ff7_submarine_patch(void) {
    if (ff7_switch_off("ff7_submarine", DATA_PATH "submarine_off.txt")) return;
    for (int i = 0; i < NUM_F; i++) {
        addrs[i] = so_symbol(&so_mod, fns[i].sym);
        if (!addrs[i] || *(uint32_t *) (addrs[i] & ~1u) != fns[i].prologue) {
            l_warn("ff7_submarine: unexpected code at %s, natives disabled", fns[i].sym);
            return;
        }
    }
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    env.check_event = (void (*)(void)) so_symbol(&so_mod, "_Z12_check_eventv");
    env.f_66e272 = (void (*)(void)) so_symbol(&so_mod, "_Z15func_0x0066e272v");
    env.f_6617e9 = (void (*)(void)) so_symbol(&so_mod, "_Z15func_0x006617e9v");
    env.f_67d2bf = (void (*)(void)) so_symbol(&so_mod, "_Z15func_0x0067d2bfv");
    env.f_66e70a = (void (*)(void)) so_symbol(&so_mod, "_Z15func_0x0066e70av");
    for (int i = 0; i < 7; i++) {
        env.f_model[i] = (void (*)(void)) so_symbol(&so_mod, model_syms[i]);
        if (!env.f_model[i]) {
            l_warn("ff7_submarine: %s not found, natives disabled", model_syms[i]);
            return;
        }
    }
    env.regs = ff7_x86_regs();
    env.fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    if (!env.mk_address || !env.check_event || !env.f_66e272 || !env.f_6617e9 || !env.f_67d2bf || !env.f_66e70a || !env.regs
        || (uintptr_t) env.fpu != so_mod.text_base + 0x1492bc0u) {
        l_warn("ff7_submarine: unexpected .so layout, natives disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event,
                              &env.memmap, &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_submarine: mk_address/_check_event not as expected, calling them (slower)");
    }
    installed = 1;
    ff7_submarine_set(1);
    l_info("ff7_submarine: native sea, bit count, splashes, surface and models (0x79F903, 0x7A2EF8, 0x79A61E,"
           " 0x7A0190, 0x79FB20) installed");
}

void ff7_submarine_set(int on) {
    static void (* const natives[NUM_F])(void) = { sea_native, bits_native, splash_native, surface_native,
                                                   model_native };
    if (!installed || on == enabled) return;
    for (int i = 0; i < NUM_F; i++) {
        if (on) {
            profiler_hold();
            hooks[i] = hook_addr(addrs[i], (uintptr_t) natives[i]);
            profiler_release();
        } else {
            ff7_unhook(&hooks[i]);
        }
    }
    enabled = on;
    l_info("ff7_submarine: natives %s", on ? "on" : "off (port menu)");
}

void ff7_submarine_report(double seconds) {
    if (seconds <= 0) return;
    if (env.surface[0] || surface_fallbacks) {
        l_info("submarine surface: %.1f frames/s, %.2f ms/frame; %.0f quads/s projected, %.0f deferred,"
               " %.0f culled; %u debug frames by original",
               env.surface[0] / seconds, surface_us / 1000.0 / (env.surface[0] + surface_fallbacks),
               env.surface[1] / seconds, env.surface[2] / seconds, env.surface[3] / seconds, surface_fallbacks);
        for (int i = 0; i < 4; i++) env.surface[i] = 0;
        surface_us = 0;
        surface_fallbacks = 0;
    }
    if (!(env.counters[0] | env.counters[1] | env.counters[2] | env.counters[3] | env.model[0])) return;
    l_info("submarine/s: %.0f sea steps, %.0f bit counts, %.0f splash triangles, %.0f water columns;"
           " %.0f models, %.0f model triangles drawn, %.0f behind the camera",
           env.counters[0] / seconds, env.counters[1] / seconds, env.counters[2] / seconds,
           env.counters[3] / seconds, env.model[0] / seconds, env.model[1] / seconds, env.model[2] / seconds);
    for (int i = 0; i < 4; i++) env.counters[i] = 0;
    for (int i = 0; i < 3; i++) env.model[i] = 0;
}
