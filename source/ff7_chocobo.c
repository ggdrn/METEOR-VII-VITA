/*
 * Native primitives of the chocobo race (game mode 7).
 *
 * 0x77B9D4 (triangle) and 0x77BCA4 (quad) transform their 3 or 4 vertices
 * by the camera matrix (0x66CE40), drop the primitive if a vertex is behind
 * the camera (w <= 0), ask 0x66E272 for room in the texture's vertex buffer
 * and write the projected vertices (x/w, y/w, z/w, 1/w, color). In the
 * v0.6.8 profile of the race they were 25% and 12% of the game thread.
 *
 * Since v0.6.12 the race's draw list runs native from the top: 0x770305
 * walks the object lists and calls 0x770767 / 0x7706D7 per object, which go
 * through 0x77BB50 / 0x77BE95 to the primitives (together ~18% of the game
 * thread in the v0.6.11 profile, mostly mk_address calls for their stack
 * frames). Only 0x770434 (type 0x21 objects) stays an x86 call.
 *
 * Since v0.6.30 a draw list loads what its primitives share once (camera
 * matrix, constants, the model's tables: ch_draw) and 0x66E272's common path
 * (room in the texture's vertex buffer) runs inline; its slow path still
 * calls the x86 function. The input vertices and the output vertex buffer are
 * resolved as one block each. Under emulation that took the list from ~1,380
 * to ~815 ARM instructions per primitive (CHOCOBO_COST=1 in the list test).
 *
 * The math is ff7_chocobo_core.h, checked by compiling it into an ARM blob
 * that runs under emulation next to the recompiled functions
 * (scripts/emu/test_chocobo.py: x86 registers, x87 state, memory and
 * _check_event calls identical over 6,000 primitives, with NaN, infinities,
 * vertices behind the camera and a null texture; scripts/emu/
 * test_chocobo_list.py for the list, object and wrapper functions over 3,000
 * random lists and calls).
 *
 * DATA_PATH "chocobo_off.txt" leaves the originals.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_chocobo.h"
#include "ff7_switch.h"
#include "ff7_chocobo_core.h"
#include "ff7_anim.h"
#include "ff7_so_inline.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH      DATA_PATH "chocobo_off.txt"
#define SYM_TRI         "_Z15func_0x0077b9d4v"
#define SYM_QUAD        "_Z15func_0x0077bca4v"
#define SYM_ALLOC       "_Z15func_0x0066e272v"
#define SYM_LIST        "_Z15func_0x00770305v"
#define SYM_OBJ_TRI     "_Z15func_0x00770767v"
#define SYM_OBJ_QUAD    "_Z15func_0x007706d7v"
#define SYM_WRAP_TRI    "_Z15func_0x0077bb50v"
#define SYM_WRAP_QUAD   "_Z15func_0x0077be95v"
#define SYM_TYPE21      "_Z15func_0x00770434v"
#define PRIM_PROLOGUE   0xb083b5f0 // push {r4-r7, lr}; sub sp, #12
#define FPU_GOT         0xf4c82c   // GOT slot: pointer to the x87 state

static chocobo_env env;
static so_hook tri_hook, quad_hook, list_hook, obj_hook[2], wrap_hook[2];
static unsigned long long fallbacks;
static uint32_t list_us; // time in the draw list (0x770305), for the report

static void tri_native(void) {
    if (!chocobo_prim_run(&env, 3)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, tri_hook);
    }
}

static void quad_native(void) {
    if (!chocobo_prim_run(&env, 4)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, quad_hook);
    }
}

static void list_native(void) {
    uint32_t t0 = sceKernelGetProcessTimeLow();
    int ok = chocobo_list_run(&env);
    list_us += sceKernelGetProcessTimeLow() - t0;
    if (!ok) {
        fallbacks++;
        SO_CONTINUE_HELD(int, list_hook);
    }
}

static void obj_tri_native(void) {
    if (!chocobo_obj_run(&env, 0)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, obj_hook[0]);
    }
}

static void obj_quad_native(void) {
    if (!chocobo_obj_run(&env, 1)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, obj_hook[1]);
    }
}

static void wrap_tri_native(void) {
    if (!chocobo_wrap_run(&env, 3)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, wrap_hook[0]);
    }
}

static void wrap_quad_native(void) {
    if (!chocobo_wrap_run(&env, 4)) {
        fallbacks++;
        SO_CONTINUE_HELD(int, wrap_hook[1]);
    }
}

// the draw list functions, with the first word of their recompiled code
static const struct {
    const char * sym;
    uint32_t prologue;
    void (* native)(void);
    so_hook * hook;
} list_funcs[] = {
    { SYM_LIST, 0xf642b5f8, list_native, &list_hook },
    { SYM_OBJ_TRI, 0x4c8db538, obj_tri_native, &obj_hook[0] },
    { SYM_OBJ_QUAD, 0x4c8cb538, obj_quad_native, &obj_hook[1] },
    { SYM_WRAP_TRI, 0x4c62b510, wrap_tri_native, &wrap_hook[0] },
    { SYM_WRAP_QUAD, 0x4c77b510, wrap_quad_native, &wrap_hook[1] },
};

void ff7_chocobo_patch(void) {
    if (ff7_switch_off("ff7_chocobo", OFF_SWITCH)) return;
    uintptr_t tri = so_symbol(&so_mod, SYM_TRI), quad = so_symbol(&so_mod, SYM_QUAD);
    env.mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    env.check_event = (void (*)(void)) so_symbol(&so_mod, "_Z12_check_eventv");
    env.f_66e272 = (void (*)(void)) so_symbol(&so_mod, SYM_ALLOC);
    env.regs = ff7_x86_regs();
    env.fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    if (!tri || !quad || !env.mk_address || !env.check_event || !env.f_66e272 || !env.regs
        || (uintptr_t) env.fpu != so_mod.text_base + 0x1492bc0u
        || *(uint32_t *) (tri & ~1) != PRIM_PROLOGUE
        || *(uint32_t *) (quad & ~1) != PRIM_PROLOGUE) {
        l_warn("ff7_chocobo: unexpected .so layout, native chocobo race primitives disabled");
        return;
    }
    if (!ff7_so_inline_lookup(&so_mod, (uintptr_t) env.mk_address, (uintptr_t) env.check_event, &env.memmap,
                              &env.ev_count)) {
        env.memmap = NULL;
        env.ev_count = NULL;
        l_warn("ff7_chocobo: mk_address/_check_event not as expected, calling them (slower)");
    }
    tri_hook = hook_addr(tri, (uintptr_t) &tri_native);
    quad_hook = hook_addr(quad, (uintptr_t) &quad_native);
    l_info("ff7_chocobo: native chocobo race triangles and quads (0x77B9D4, 0x77BCA4) installed");

    env.f_770434 = (void (*)(void)) so_symbol(&so_mod, SYM_TYPE21);
    uintptr_t addr[sizeof(list_funcs) / sizeof(list_funcs[0])];
    for (unsigned i = 0; i < sizeof(list_funcs) / sizeof(list_funcs[0]); i++) {
        addr[i] = so_symbol(&so_mod, list_funcs[i].sym);
        if (!env.f_770434 || !addr[i] || *(uint32_t *) (addr[i] & ~1) != list_funcs[i].prologue) {
            l_warn("ff7_chocobo: unexpected code at %s, native chocobo race draw list disabled", list_funcs[i].sym);
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(list_funcs) / sizeof(list_funcs[0]); i++)
        *list_funcs[i].hook = hook_addr(addr[i], (uintptr_t) list_funcs[i].native);
    l_info("ff7_chocobo: native chocobo race draw list (0x770305, 0x770767, 0x7706D7, 0x77BB50, 0x77BE95) installed");
}

void ff7_chocobo_report(double seconds) {
    if (seconds <= 0 || (!env.counters[0] && !env.counters[1] && !env.counters[3] && !fallbacks)) return;
    l_info("chocobo race/s: %.0f triangles, %.0f quads native (%.0f drawn), %.0f lists, %.0f objects, %.0f original;"
           " draw list %.1f ms/s",
           env.counters[0] / seconds, env.counters[1] / seconds, env.counters[2] / seconds, env.counters[3] / seconds,
           env.counters[4] / seconds, fallbacks / seconds, list_us / 1000.0 / seconds);
    list_us = 0;
    for (int i = 0; i < 5; i++) env.counters[i] = 0;
    fallbacks = 0;
}
