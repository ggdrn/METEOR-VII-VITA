/*
 * Native functions of the snowboard minigame (game modes 8 and 14).
 *
 * In the v0.6.16 profile of a whole snowboard run (game thread 96% busy,
 * 16-24 fps) three functions and their mk_address calls took ~65% of the
 * game thread:
 *   0x7335FB (clip a triangle at z = 0.5)   14.5% + 12.9% mk_address
 *   0x73488D (clip + write the triangles)   13.1% + 13.6%
 *   0x7334DA (vertex * matrix)               8.3% +  2.9%
 * 0x73488D runs native with 0x7335FB, its color interpolation 0x73404C and
 * their _ftol calls inline (0x7335FB is only called from it); its wrapper
 * 0x734CAB (2.2% + 0.9%) calls that native work directly; 0x7334DA runs
 * native on its own. 0x66E272 stays an x86 call (the native ff7_engine hook).
 *
 * The math is ff7_snowboard_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled functions
 * (scripts/emu/test_snowboard.py: x86 registers, x87 state, memory and
 * _check_event calls identical over 6,000 calls of the three, with every
 * clip case, NaN, infinities, w = 0 and a null texture).
 *
 * DATA_PATH "snowboard_off.txt" leaves the originals.
 *
 * 30 fps cap (v0.6.19): with the natives the run reaches 60 fps, which keeps
 * the CPU at 96%. The snowboard's frame (0x72381C, once per drawn frame) runs
 * fixed 60 Hz logic steps for the time measured since the last frame
 * (0x723960: elapsed * 60 into [[0x926290] + 0x268], up to 32 steps) and
 * draws once, so drawing at 30 fps runs 2 logic steps a frame: same speed.
 * Its entry waits until 33.3 ms have passed since the previous frame.
 * DATA_PATH "snowboard_60fps.txt" leaves it uncapped.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_snowboard.h"
#include "ff7_switch.h"
#include "ff7_snowboard_core.h"
#include "ff7_anim.h"
#include "ff7_so_inline.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH      DATA_PATH "snowboard_off.txt"
#define SYM_XFORM       "_Z15func_0x007334dav"
#define SYM_TRI         "_Z15func_0x0073488dv"
#define SYM_TRI3        "_Z15func_0x00734cabv"
#define TRI3_PROLOGUE   0x4ca6b510 // push {r4, lr}; ldr r4, [pc, #...]
#define SYM_ALLOC       "_Z15func_0x0066e272v"
#define PROLOGUE        0x4ff0e92d // stmdb sp!, {r4-r11, lr}
#define FPU_GOT         0xf4c82c   // GOT slot: pointer to the x87 state

static snowboard_env env;
static so_hook xform_hook, tri_hook, tri3_hook, frame_hook;
static unsigned long long fallbacks;

#define SYM_FRAME       "_Z15func_0x0072381cv"
#define CAP_OFF         DATA_PATH "snowboard_60fps.txt"
#define FRAME_US        33333 // 30 fps

static SceUInt64 next_frame;
static unsigned capped_frames, cap_wait_us;

static void frame_native(void) {
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (next_frame && now < next_frame) {
        SceUInt64 wait = next_frame - now;
        if (wait > FRAME_US) wait = FRAME_US;
        sceKernelDelayThread((SceUInt) wait);
        cap_wait_us += (unsigned) wait;
        now = sceKernelGetProcessTimeWide();
    }
    // a steady 30 fps cadence; after a slow frame, start over from now
    next_frame = next_frame && now - next_frame < FRAME_US ? next_frame + FRAME_US : now + FRAME_US;
    capped_frames++;
    SO_CONTINUE_HELD(int, frame_hook);
}

static void xform_native(void) {
    if (!snowboard_xform_run(&env)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, xform_hook);
    }
}

static void tri_native(void) {
    if (!snowboard_tri_run(&env)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, tri_hook);
    }
}

static void tri3_native(void) {
    if (!snowboard_tri3_run(&env)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, tri3_hook);
    }
}

static void frame_cap_patch(void) {
    if (file_exists(CAP_OFF)) {
        l_info("ff7_snowboard: 30 fps cap disabled by %s", CAP_OFF);
        return;
    }
    uintptr_t frame = so_symbol(&so_mod, SYM_FRAME);
    if (!frame) {
        l_warn("ff7_snowboard: %s not found, no 30 fps cap", SYM_FRAME);
        return;
    }
    frame_hook = hook_addr(frame, (uintptr_t) &frame_native);
    l_info("ff7_snowboard: snowboard drawn at 30 fps at most (logic stays 60 Hz)");
}

void ff7_snowboard_patch(void) {
    frame_cap_patch();
    if (ff7_switch_off("ff7_snowboard", OFF_SWITCH)) return;
    uintptr_t xform = so_symbol(&so_mod, SYM_XFORM), tri = so_symbol(&so_mod, SYM_TRI);
    uintptr_t tri3 = so_symbol(&so_mod, SYM_TRI3);
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    env.check_event = (void (*)(void)) so_symbol(&so_mod, "_Z12_check_eventv");
    env.f_66e272 = (void (*)(void)) so_symbol(&so_mod, SYM_ALLOC);
    env.regs = ff7_x86_regs();
    env.fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    if (!xform || !tri || !tri3 || !env.mk_address || !env.check_event || !env.f_66e272 || !env.regs
        || (uintptr_t) env.fpu != so_mod.text_base + 0x1492bc0u
        || *(uint32_t *) (xform & ~1) != PROLOGUE || *(uint32_t *) (tri & ~1) != PROLOGUE
        || *(uint32_t *) (tri3 & ~1) != TRI3_PROLOGUE) {
        l_warn("ff7_snowboard: unexpected .so layout, native snowboard functions disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event, &env.memmap,
                              &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_snowboard: mk_address/_check_event not as expected, calling them (slower)");
    }
    xform_hook = hook_addr(xform, (uintptr_t) &xform_native);
    tri_hook = hook_addr(tri, (uintptr_t) &tri_native);
    tri3_hook = hook_addr(tri3, (uintptr_t) &tri3_native);
    l_info("ff7_snowboard: native snowboard transform and triangle clip (0x7334DA, 0x73488D + 0x7335FB, 0x734CAB)"
           " installed");
}

void ff7_snowboard_report(double seconds) {
    if (seconds > 0 && capped_frames) {
        l_info("snowboard cap: %.1f frames/s, %.1f ms/s waiting for 30 fps", capped_frames / seconds,
               cap_wait_us / 1000.0 / seconds);
        capped_frames = cap_wait_us = 0;
    }
    if (seconds <= 0 || (!env.counters[0] && !env.counters[1] && !fallbacks)) return;
    l_info("snowboard/s: %.0f transforms, %.0f triangles (%.0f clipped, %.0f drawn) native, %.0f original",
           env.counters[0] / seconds, env.counters[1] / seconds, env.counters[2] / seconds,
           env.counters[3] / seconds, fallbacks / seconds);
    for (int i = 0; i < 4; i++) env.counters[i] = 0;
    fallbacks = 0;
}
