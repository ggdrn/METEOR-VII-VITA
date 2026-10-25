/*
 * Native shared 3D engine conversions: PSX fixed point to float.
 *
 * Every 3D mode converts PSX-style matrices and vectors before drawing:
 * 0x661465 (matrix, with 0x6611DA for its translation) and 0x661098
 * (vector) are reached from the motorbike, chocobo, Fort Condor, submarine
 * and roller coaster minigames, battle and the world map (docs/MINIGAMES.md).
 * In the recompiled code each value goes through the emulated x87 stack
 * (fild / fdiv / fstp) and several mk_address calls; in the v0.6.0 profile
 * of the motorbike they summed ~4.6% of the game thread.
 *
 * 0x66E272 (room for n primitives in a texture's vertex buffer) is called
 * for every primitive the 3D minigames draw: 11-19% of the game thread in
 * the snowboard, chocobo race and roller coaster profiles (v0.6.7). Its
 * common path (the buffer is current and has room) is native; a new frame
 * (buffer reset) or a full buffer (a new one is allocated) still runs the
 * original, a few times per texture and frame.
 *
 * The math is ff7_engine_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled functions
 * (scripts/emu/test_engine.py: x86 registers, x87 state, the pushed value,
 * memory and _check_event calls identical over 9,000 calls, including
 * divisions by zero and values across page boundaries).
 *
 * With DATA_PATH "selftest_on.txt" the first calls also run the original and
 * compare; on a mismatch that hook is removed. DATA_PATH "engine_off.txt"
 * leaves the originals.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_engine.h"
#include "ff7_switch.h"
#include "ff7_engine_core.h"
#include "ff7_anim.h"
#include "ff7_selftest.h"
#include "ff7_so_inline.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/profiler.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH      DATA_PATH "engine_off.txt"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define FPU_GOT         0xf4c82c // GOT slot: pointer to the x87 state
#define SELFTEST_CALLS  500
#define ALLOC_PROLOGUE  0xf8dfb538 // push {r3-r5, lr}; ldr.w ...

typedef int (* run_fn)(engine_env * e);

typedef struct {
    const char * sym;
    uint32_t prologue;
    run_fn run;
    uint32_t out_size; // bytes written at the 2nd argument (0: the x87 stack)
    // run() returns 1 (done), 0 (cannot: the original runs, counted as a
    // fallback) or -1 (a case left to the original by design: counted apart)
    const char * name;
    so_hook hook;
    int selftest_left, disabled;
    unsigned long long fallbacks, originals;
} engine_fn;

static engine_env env;
#define NUM_FNS 4
static engine_fn fns[NUM_FNS] = {
    { "_Z15func_0x00661465v", 0x41f0e92d, engine_matrix_run, 0x40, "0x661465 (matrix)" },
    { "_Z15func_0x006611dav", 0xb082b570, engine_fixdiv_run, 0, "0x6611DA (fixed point)" },
    { "_Z15func_0x00661098v", 0xb083b5f0, engine_vector_run, 12, "0x661098 (vector)" },
    { "_Z15func_0x0066e272v", ALLOC_PROLOGUE, engine_alloc_run, 0x90, "0x66E272 (vertex buffer)" },
};

/*
 * 0x66E272's original without SO_CONTINUE: its slow path (a new frame resets
 * the buffer, or a full one gets a new one) runs ~2,800 times/s in the
 * chocobo race, and each SO_CONTINUE rewrites the entry and flushes the caches
 * twice. The hook covers its first 8 bytes:
 *     push {r3, r4, r5, lr}; ldr.w r4, [pc, #imm]; add r4, pc
 * so the trampoline does the push, puts in r4 what the two others would
 * (literal + the add's PC) and jumps to entry + 8.
 */
uintptr_t ff7_engine_alloc_r4, ff7_engine_alloc_resume;
static int alloc_tramp_ok;

__attribute__((naked)) static void alloc_original(void) {
    __asm__ volatile(
        "push {r3, r4, r5, lr}\n\t"
        "ldr r4, =ff7_engine_alloc_r4\n\t"
        "ldr r4, [r4]\n\t"
        "ldr r12, =ff7_engine_alloc_resume\n\t"
        "ldr r12, [r12]\n\t"
        "bx r12\n\t"
        ".ltorg\n\t");
}

static int alloc_tramp_setup(uintptr_t fn) {
    const uint16_t * hw = (const uint16_t *) (fn & ~1);
    // push {r3, r4, r5, lr}; ldr.w r4, [pc, #imm12]; add r4, pc
    if (hw[0] != 0xb538 || hw[1] != 0xf8df || (hw[2] & 0xf000) != 0x4000 || hw[3] != 0x447c) return 0;
    uintptr_t entry = (uintptr_t) hw;
    uintptr_t literal = ((entry + 2 + 4) & ~3u) + (hw[2] & 0xfff);
    ff7_engine_alloc_r4 = *(uint32_t *) literal + entry + 6 + 4;
    ff7_engine_alloc_resume = (entry + 8) | 1;
    return 1;
}

static void original(engine_fn * f) {
    if (f == &fns[3] && alloc_tramp_ok) {
        alloc_original();
        return;
    }
    profiler_hold();
    SO_CONTINUE(int, f->hook);
    profiler_release();
}

static void native(engine_fn * f) {
    if (f->disabled) { // a call already on its way when the hook was removed
        original(f);
        return;
    }
    if (f->selftest_left > 0) {
        volatile uint32_t * regs = env.regs;
        uint32_t r0[8], rn[8], out = 0;
        uint8_t f0[0x68], fn[0x68], before[0x40], mine[0x40], orig[0x40];
        for (int i = 0; i < 8; i++) r0[i] = regs[i];
        memcpy(f0, env.fpu, sizeof(f0));
        if (f->out_size) {
            memcpy(&out, env.mk_address(r0[4] + 8), 4);
            for (uint32_t i = 0; i < f->out_size; i++) before[i] = *env.mk_address(out + i);
        }
        int r = f->run(&env);
        if (r <= 0) {
            if (r < 0) f->originals++;
            else f->fallbacks++;
            original(f);
            return;
        }
        for (int i = 0; i < 8; i++) rn[i] = regs[i];
        memcpy(fn, env.fpu, sizeof(fn));
        for (uint32_t i = 0; i < f->out_size; i++) mine[i] = *env.mk_address(out + i);
        for (int i = 0; i < 8; i++) regs[i] = r0[i];
        memcpy(env.fpu, f0, sizeof(f0));
        for (uint32_t i = 0; i < f->out_size; i++) *env.mk_address(out + i) = before[i];
        original(f);
        for (uint32_t i = 0; i < f->out_size; i++) orig[i] = *env.mk_address(out + i);
        uint32_t top;
        memcpy(&top, env.fpu + EN_TOP, 4);
        int ok = !memcmp(mine, orig, f->out_size) && !memcmp(fn + 0x40, env.fpu + 0x40, 0x68 - 0x40);
        if (!f->out_size && top <= 7) ok = ok && !memcmp(fn + 8 * top, env.fpu + 8 * top, 8);
        for (int i = 0; i < 8; i++) if (rn[i] != regs[i]) ok = 0;
        f->selftest_left--;
        if (!ok) {
            l_error("ff7_engine %s selftest MISMATCH (eax %08x/%08x ecx %08x/%08x edx %08x/%08x) - hook removed",
                    f->name, rn[0], regs[0], rn[1], regs[1], rn[2], regs[2]);
            f->disabled = 1;
            ff7_unhook(&f->hook);
        } else if (!f->selftest_left) {
            l_info("ff7_engine %s selftest ok: %d calls identical", f->name, SELFTEST_CALLS);
        }
        return;
    }
    int r = f->run(&env);
    if (r <= 0) {
        if (r < 0) f->originals++;
        else f->fallbacks++;
        original(f);
    }
}

static void matrix_native(void) { native(&fns[0]); }
static void fixdiv_native(void) { native(&fns[1]); }
static void vector_native(void) { native(&fns[2]); }
static void alloc_native(void) { native(&fns[3]); }

void ff7_engine_patch(void) {
    if (ff7_switch_off("ff7_engine", OFF_SWITCH)) return;
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, SYM_MK_ADDRESS);
    env.check_event = (void (*)(void)) so_symbol(&so_mod, SYM_CHECK_EVENT);
    env.regs = ff7_x86_regs();
    env.fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    if (!env.mk_address || !env.check_event || !env.regs || (uintptr_t) env.fpu != so_mod.text_base + 0x1492bc0u) {
        l_warn("ff7_engine: unexpected .so layout, native conversions disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event, &env.memmap,
                              &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_engine: mk_address/_check_event not as expected, calling them (slower)");
    }
    static void (* const handlers[NUM_FNS])(void) = { matrix_native, fixdiv_native, vector_native, alloc_native };
    for (int i = 0; i < NUM_FNS; i++) {
        engine_fn * f = &fns[i];
        uintptr_t fn = so_symbol(&so_mod, f->sym);
        if (!fn || *(uint32_t *) (fn & ~1) != f->prologue) {
            // 0x66E272 is also hooked by ff7_bg while its self-test runs
            l_warn("ff7_engine: unexpected code at %s (%08x), left alone", f->name,
                   fn ? *(uint32_t *) (fn & ~1) : 0);
            continue;
        }
        if (f == &fns[3]) alloc_tramp_ok = alloc_tramp_setup(fn);
        f->selftest_left = ff7_selftest_count(SELFTEST_CALLS);
        f->hook = hook_addr(fn, (uintptr_t) handlers[i]);
        l_info("ff7_engine: native %s installed (selftest on %d calls)%s", f->name, f->selftest_left,
               f == &fns[3] ? (alloc_tramp_ok ? ", original by trampoline" : ", original by SO_CONTINUE") : "");
    }
}

void ff7_engine_report(double seconds) {
    if (seconds <= 0) return;
    unsigned long long fb = fns[0].fallbacks + fns[1].fallbacks + fns[2].fallbacks + fns[3].fallbacks;
    if (!env.counters[0] && !env.counters[1] && !env.counters[2] && !env.counters[3] && !fb && !fns[3].originals)
        return;
    l_info("engine/s: %.0f matrices, %.0f fixed point, %.0f vectors, %.0f vertex buffer room native; %.0f buffer"
           " allocations by the original; %.0f fallbacks", env.counters[0] / seconds, env.counters[1] / seconds,
           env.counters[2] / seconds, env.counters[3] / seconds, fns[3].originals / seconds, fb / seconds);
    for (int i = 0; i < NUM_FNS; i++) {
        env.counters[i] = 0;
        fns[i].fallbacks = fns[i].originals = 0;
    }
}
