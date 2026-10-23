/*
 * Native functions of the roller coaster minigame (game mode 11).
 *
 * In the v0.6.23 profile of a whole ride (game thread 96% busy, 18-60 fps)
 * the coaster's own triangle code took most of the game thread:
 *   0x5F2639 (project a vertex)         16.6% + 2.4% mk_address
 *   0x5F04D7 (one triangle)             15.4% + 3.0%
 *   _ftol (0x40B470, 9 a triangle)      14.8%
 *   0x5F2759 (vertex depth)             11.5% + 2.4%
 *   0x5F2838 (distance fade)             4.6%
 *   0x5F2897 (fade a color, 3 _ftol)     2.6%
 * 0x5F04D7 runs native with the four others and _ftol inline; its clipping
 * path (a vertex closer than -100 in front of the camera) and NaN depths go
 * to the original through a trampoline (no SO_CONTINUE). The four helpers,
 * also called from elsewhere, are native on their own too.
 *
 * The math is ff7_coaster_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled functions
 * (scripts/emu/test_coaster.py).
 *
 * DATA_PATH "coaster_off.txt" leaves the originals.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_coaster.h"
#include "ff7_coaster_core.h"
#include "ff7_anim.h"
#include "ff7_so_inline.h"
#include "ff7_switch.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH  DATA_PATH "coaster_off.txt"
#define SYM_ALLOC   "_Z15func_0x0066e272v"
#define FPU_GOT     0xf4c82c // GOT slot: pointer to the x87 state

static coaster_env env;
static unsigned long long fallbacks, clip_paths;

enum { F_TRI, F_PROJECT, F_DEPTH, F_FADE, F_COLOR, NUM_F };
static const struct {
    const char * sym;
    uint32_t prologue;
} fns[NUM_F] = {
    { "_Z15func_0x005f04d7v", 0x43f0e92d }, // stmdb sp!, {r4-r9, lr}
    { "_Z15func_0x005f2639v", 0xb083b5f0 }, // push {r4-r7, lr}; sub sp, #12
    { "_Z15func_0x005f2759v", 0xb083b5f0 },
    { "_Z15func_0x005f2838v", 0x41f0e92d }, // stmdb sp!, {r4-r8, lr}
    { "_Z15func_0x005f2897v", 0xb083b5f0 },
};
static so_hook hooks[NUM_F];
static uintptr_t addrs[NUM_F];
static int installed, enabled;

/*
 * 0x5F04D7's original without SO_CONTINUE (its clipping path runs whenever a
 * coaster triangle comes close): the hook covers
 *     stmdb sp!, {r4-r9, lr}; sub sp, #12; ldr.w r5, [pc, #imm] (half of it)
 * so the trampoline does the first two, loads r5 with the literal and goes
 * on at entry + 10.
 */
uintptr_t ff7_coaster_tri_r5, ff7_coaster_tri_resume;
static int tri_tramp_ok;

__attribute__((naked)) static void tri_original(void) {
    __asm__ volatile(
        "push {r4, r5, r6, r7, r8, r9, lr}\n\t"
        "sub sp, sp, #12\n\t"
        "ldr r5, =ff7_coaster_tri_r5\n\t"
        "ldr r5, [r5]\n\t"
        "ldr r12, =ff7_coaster_tri_resume\n\t"
        "ldr r12, [r12]\n\t"
        "bx r12\n\t"
        ".ltorg\n\t");
}

static int tri_tramp_setup(uintptr_t fn) {
    const uint16_t * hw = (const uint16_t *) (fn & ~1);
    // stmdb sp!, {r4-r9, lr}; sub sp, #12; ldr.w r5, [pc, #imm12]
    if (hw[0] != 0xe92d || hw[1] != 0x43f0 || hw[2] != 0xb083 || hw[3] != 0xf8df || (hw[4] & 0xf000) != 0x5000)
        return 0;
    uintptr_t entry = (uintptr_t) hw;
    uintptr_t literal = ((entry + 6 + 4) & ~3u) + (hw[4] & 0xfff);
    ff7_coaster_tri_r5 = *(uint32_t *) literal;
    ff7_coaster_tri_resume = (entry + 10) | 1;
    return 1;
}

static void tri_native(void) {
    if (coaster_tri_run(&env)) return;
    clip_paths++;
    if (tri_tramp_ok) {
        tri_original();
    } else {
        SO_CONTINUE_HELD(int, hooks[F_TRI]);
    }
}

#define HELPER(name, run, idx) \
    static void name(void) { \
        if (!run(&env)) { \
            fallbacks++; \
            SO_CONTINUE_HELD(int, hooks[idx]); \
        } \
    }
HELPER(project_native, coaster_project_run, F_PROJECT)
HELPER(depth_native, coaster_depth_run, F_DEPTH)
HELPER(fade_native, coaster_fade_run, F_FADE)
HELPER(color_native, coaster_color_run, F_COLOR)

void ff7_coaster_patch(void) {
    if (ff7_switch_off("ff7_coaster", OFF_SWITCH)) return;
    uintptr_t addr[NUM_F];
    for (int i = 0; i < NUM_F; i++) {
        addr[i] = so_symbol(&so_mod, fns[i].sym);
        if (!addr[i] || *(uint32_t *) (addr[i] & ~1) != fns[i].prologue) {
            l_warn("ff7_coaster: unexpected code at %s, native roller coaster disabled", fns[i].sym);
            return;
        }
    }
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    env.check_event = (void (*)(void)) so_symbol(&so_mod, "_Z12_check_eventv");
    env.f_66e272 = (void (*)(void)) so_symbol(&so_mod, SYM_ALLOC);
    env.regs = ff7_x86_regs();
    env.fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    if (!env.mk_address || !env.check_event || !env.f_66e272 || !env.regs
        || (uintptr_t) env.fpu != so_mod.text_base + 0x1492bc0u) {
        l_warn("ff7_coaster: unexpected .so layout, native roller coaster disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event, &env.memmap,
                              &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_coaster: mk_address/_check_event not as expected, calling them (slower)");
    }
    tri_tramp_ok = tri_tramp_setup(addr[F_TRI]);
    for (int i = 0; i < NUM_F; i++) addrs[i] = addr[i];
    installed = 1;
    ff7_coaster_set(1);
    l_info("ff7_coaster: native roller coaster triangle, projection, depth, fade and color (0x5F04D7, 0x5F2639,"
           " 0x5F2759, 0x5F2838, 0x5F2897) installed; clipping path by %s",
           tri_tramp_ok ? "trampoline" : "SO_CONTINUE");
}

// A/B test from the port menu: the hooks put in or taken out (game thread,
// between frames: none of these functions is running)
void ff7_coaster_set(int on) {
    static void (* const natives[NUM_F])(void) = { tri_native, project_native, depth_native, fade_native,
                                                   color_native };
    if (!installed || on == enabled) return;
    for (int i = 0; i < NUM_F; i++) {
        if (on) {
            profiler_hold(); // the profiler changes the text's permissions from another core
            hooks[i] = hook_addr(addrs[i], (uintptr_t) natives[i]);
            profiler_release();
        } else {
            ff7_unhook(&hooks[i]);
        }
    }
    enabled = on;
    l_info("ff7_coaster: natives %s", on ? "on" : "off (port menu)");
}

void ff7_coaster_report(double seconds) {
    if (seconds <= 0 || (!env.counters[0] && !env.counters[1] && !env.counters[2] && !clip_paths && !fallbacks))
        return;
    l_info("coaster/s: %.0f triangles drawn, %.0f not in front, %.0f clipped by the original; %.0f projections,"
           " %.0f colors; %.0f fallbacks", env.counters[0] / seconds, env.counters[1] / seconds,
           clip_paths / seconds, env.counters[2] / seconds, env.counters[3] / seconds, fallbacks / seconds);
    for (int i = 0; i < 4; i++) env.counters[i] = 0;
    clip_paths = fallbacks = 0;
}
