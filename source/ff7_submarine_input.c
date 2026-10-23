/* Deliver physical Square edges on the submarine game thread. Keep the
 * original torpedo availability/player-state checks and firing routine.
 * Trampolines replay the checked first 8 bytes without rewriting hooks.
 */
#include "ff7_submarine.h"
#include "ff7_anim.h"
#include "ff7_unhook.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include <stdint.h>
#include <so_util/so_util.h>

extern so_module so_mod;
static uint8_t * const * pages;
static volatile unsigned pending;
static unsigned presses, delivered, attempts, fired, no_tube, no_slot, no_target;
static int installed;
uintptr_t ff7_sm_input_regs, ff7_sm_input_resume, ff7_sm_fire_resume;

__attribute__((naked)) static void input_original(void) {
    __asm__ volatile(
        "push {r4, r5, r6, lr}\n\t"
        "ldr r4, =ff7_sm_input_regs\n\t"
        "ldr r4, [r4]\n\t"
        "ldr r12, =ff7_sm_input_resume\n\t"
        "ldr r12, [r12]\n\t"
        "bx r12\n\t.ltorg\n\t");
}
__attribute__((naked)) static void fire_original(void) {
    __asm__ volatile(
        "push {r3, r4, r5, r6, r7, lr}\n\t"
        "ldr r4, =ff7_sm_input_regs\n\t"
        "ldr r4, [r4]\n\t"
        "ldr r12, =ff7_sm_fire_resume\n\t"
        "ldr r12, [r12]\n\t"
        "bx r12\n\t.ltorg\n\t");
}
static uint32_t * word(uint32_t va) {
    uint8_t * p = pages ? pages[va >> 12] : NULL;
    return p ? (uint32_t *) (p + (va & 0xfff)) : NULL;
}
static unsigned ready_tubes(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < 4; i++) n += *word(0x9873e4u + 4 * i) == 0x20000;
    return n;
}
static void fire_native(void) {
    unsigned ready = ready_tubes(), slots = 0;
    for (unsigned i = 0; i < 16; i++) slots += *word(0xec18b4u + 0xc8 * i) == 1;
    __sync_fetch_and_add(&attempts, 1);
    if (!ready) __sync_fetch_and_add(&no_tube, 1);
    else if (!slots) __sync_fetch_and_add(&no_slot, 1);
    else {
        int target = 0;
        for (unsigned i = 0; i < 12; i++) target |= (*word(0x98a1e0u + 0x70 * i) & 0x800) != 0;
        if (!target) __sync_fetch_and_add(&no_target, 1);
    }
    fire_original();
    unsigned after = ready_tubes();
    if (after < ready) __sync_fetch_and_add(&fired, ready - after);
}
static void input_native(void) {
    uint32_t * edges = word(0x9a85e0u);
    unsigned n;
    int inject = 0;
    do {
        n = __sync_fetch_and_add(&pending, 0);
        if (!n) break;
        inject = __sync_bool_compare_and_swap(&pending, n, n - 1);
    } while (!inject);
    uint32_t saved = *edges;
    // Physical Square uses this queue exclusively; virtual/touch input
    // keeps its original Android path and cannot duplicate a physical edge.
    if (inject) { *edges = saved | 0x80; __sync_fetch_and_add(&delivered, 1); }
    input_original();
    *edges = saved;
}
void ff7_submarine_input_poll(void) {
    if (!installed) return;
    uint32_t * mode = word(0xcbf9dcu);
    if (!mode || (*(uint16_t *) mode != 10)) __sync_lock_test_and_set(&pending, 0);
}
int ff7_submarine_input_key(int down) {
    if (!installed) return 0;
    uint32_t * mode = word(0xcbf9dcu);
    if (!mode || (*(uint16_t *) mode != 10)) {
        __sync_lock_test_and_set(&pending, 0);
        return 0;
    }
    if (down) {
        __sync_fetch_and_add(&pending, 1);
        __sync_fetch_and_add(&presses, 1);
    }
    return 1;
}
static int trampoline_ok(uintptr_t fn, uint16_t push, uint16_t load) {
    uintptr_t a = fn & ~1u;
    const uint16_t * code = (const uint16_t *) a;
    if (code[0] != push || code[1] != load || code[2] != 0x447c || code[3] != 0x6824) return 0;
    uintptr_t literal = ((a + 6) & ~3u) + ((load & 0xff) << 2);
    uintptr_t regs = *(uint32_t *) (a + 8 + *(uint32_t *) literal);
    return regs == ff7_sm_input_regs;
}
void ff7_submarine_input_patch(void) {
    if (file_exists(DATA_PATH "submarine_input_off.txt")) return;
    uintptr_t input = so_symbol(&so_mod, "_Z15func_0x00798580v");
    uintptr_t fire = so_symbol(&so_mod, "_Z15func_0x0079f114v");
    pages = (uint8_t * const *) so_symbol(&so_mod, "MEMMAP");
    ff7_sm_input_regs = (uintptr_t) ff7_x86_regs();
    if (!input || !fire || !pages || !ff7_sm_input_regs
        || !trampoline_ok(input, 0xb570, 0x4c23) || !trampoline_ok(fire, 0xb5f8, 0x4c75)) {
        l_warn("submarine input: unexpected code, Square bridge disabled");
        return;
    }
    ff7_sm_input_resume = ((input & ~1u) + 8) | 1;
    ff7_sm_fire_resume = ((fire & ~1u) + 8) | 1;
    profiler_hold();
    hook_addr(input, (uintptr_t) input_native);
    hook_addr(fire, (uintptr_t) fire_native);
    profiler_release();
    installed = 1;
    l_info("submarine input: Square queued to fire edge 0x80 on the game thread; original torpedo checks kept");
}
void ff7_submarine_input_report(void) {
    unsigned p = __sync_lock_test_and_set(&presses, 0);
    unsigned d = __sync_lock_test_and_set(&delivered, 0), a = __sync_lock_test_and_set(&attempts, 0);
    unsigned f = __sync_lock_test_and_set(&fired, 0), nt = __sync_lock_test_and_set(&no_tube, 0);
    unsigned ns = __sync_lock_test_and_set(&no_slot, 0), nr = __sync_lock_test_and_set(&no_target, 0);
    if (!(p | d | a | f | nt | ns | nr)) return;
    l_info("submarine fire: %u Square presses, %u edges delivered, %u attempts, %u torpedoes fired;"
           " rejected: %u tubes reloading, %u no free slot, %u no eligible target; %u pending; phase %u",
           p, d, a, f, nt, ns, nr, __sync_fetch_and_add(&pending, 0), *word(0xe74768u));
}
