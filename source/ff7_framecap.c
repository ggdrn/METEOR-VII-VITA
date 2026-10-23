/*
 * 30 fps cap for minigames with fixed 60 Hz logic.
 *
 * The snowboard and the roller coaster measure the time since their last
 * frame, add it in 1/60 s units to a counter (up to 32 steps) and run one
 * logic step per unit, drawing only on the last one:
 *
 *   snowboard   frame 0x72381C: 0x723960 adds elapsed * 60 to
 *               [[0x926290] + 0x268]; steps [0xDD7CEC], draw flag +0x270
 *   coaster     frame 0x5E8E7E (the mode's main, once per frame): 0x5E8F9B
 *               adds elapsed * 60 to [0xC3F6E8]; steps 0x5E9051, draw flag
 *               [0x9014A8]
 *
 * Drawing at 30 fps thus runs 2 logic steps a frame and the game keeps its
 * speed. Run as fast as they can, both reach 40-60 fps in light scenes and
 * keep the game thread at 96% (the logs of v0.6.18 and v0.6.23): capped, the
 * spare time goes to the heavy scenes. The entry of each frame function
 * waits until 33.3 ms have passed since the previous frame; a slow frame
 * restarts the cadence instead of hurrying the next ones.
 *
 * The submarine (mode 10) already runs 30 Hz logic, but its limiter 0x78C9E1
 * spins until (ticks / 30 - 10000), ~32.3 ms, after the start of the last
 * logic step, and only then draws; drawing and the hand-off to the render
 * thread (which waits for a vblank) come on top, so a frame took ~36 ms and
 * the minigame ran at 27.5 fps with the game thread 60% idle (v0.6.27 log).
 * Its limiter is replaced with the same 33.3 ms cadence: called after a
 * draw (flag [0xE73F18] set), it sleeps until the next step's time and asks
 * for a logic step (as its "late" path: flag 0, [game + 0x40] += 1.0,
 * [0xE747A0] = 0); called after a logic step, it asks for the drawing (flag
 * 1), or for one catch-up step when the next one is already due (as the
 * original, at most one: [0xE747A0]).
 *
 * DATA_PATH "snowboard_60fps.txt" / "coaster_60fps.txt" leave one uncapped,
 * "submarine_limiter_off.txt" keeps the submarine's own limiter.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_framecap.h"
#include "ff7_anim.h"
#include "ff7_unhook.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define FRAME_US 33333 // 30 fps

static volatile int cap_on = 1; // port menu A/B test

typedef struct {
    const char * name, * sym, * off_switch;
    so_hook hook;
    SceUInt64 next;
    unsigned frames, wait_us;
} capped_t;

static capped_t caps[] = {
    { "snowboard", "_Z15func_0x0072381cv", DATA_PATH "snowboard_60fps.txt" },
    { "coaster", "_Z15func_0x005e8e7ev", DATA_PATH "coaster_60fps.txt" },
};
#define NUM_CAPS ((int) (sizeof(caps) / sizeof(caps[0])))

void port_menu_apply_ab(void);

static void cap_wait(capped_t * c) {
    port_menu_apply_ab(); // between two frames of the minigame: a safe point
    if (!cap_on) {
        c->next = 0;
        return;
    }
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (c->next && now < c->next) {
        SceUInt64 wait = c->next - now;
        if (wait > FRAME_US) wait = FRAME_US;
        sceKernelDelayThread((SceUInt) wait);
        c->wait_us += (unsigned) wait;
        now = sceKernelGetProcessTimeWide();
    }
    // a steady 30 fps cadence; after a slow frame, start over from now
    c->next = c->next && now - c->next < FRAME_US ? c->next + FRAME_US : now + FRAME_US;
    c->frames++;
}

// The submarine's limiter 0x78C9E1 (game): cdecl, x86 memory through MEMMAP
#define SUB_FLAG   0xe73f18u // 0: run a logic step, 1: draw
#define SUB_LATE   0xe74328u // double: overshoot of the last step (only the limiter reads it)
#define SUB_CATCH  0xe747a0u // a catch-up step was taken
#define SUB_LIMITER_OFF DATA_PATH "submarine_limiter_off.txt"

static so_hook sub_hook;
static uint8_t * const * sub_pages;
static SceUInt64 sub_due;
static unsigned sub_frames, sub_steps, sub_catchups, sub_wait_us;
uintptr_t ff7_framecap_sub_r4, ff7_framecap_sub_resume;

static void * sub_mem(uint32_t va) {
    uint8_t * p = sub_pages[va >> 12];
    return p ? p + (va & 0xfff) : NULL;
}

/*
 * The original limiter without SO_CONTINUE (port menu A/B): the hook covers
 *     stmdb sp!, {r4-r9, lr}; sub sp, #12; ldr.w r4, [pc, #imm] (half of it)
 */
__attribute__((naked)) static void sub_original(void) {
    __asm__ volatile(
        "push {r4, r5, r6, r7, r8, r9, lr}\n\t"
        "sub sp, sp, #12\n\t"
        "ldr r4, =ff7_framecap_sub_r4\n\t"
        "ldr r4, [r4]\n\t"
        "ldr r12, =ff7_framecap_sub_resume\n\t"
        "ldr r12, [r12]\n\t"
        "bx r12\n\t"
        ".ltorg\n\t");
}

static int sub_tramp_setup(uintptr_t fn) {
    const uint16_t * hw = (const uint16_t *) (fn & ~1);
    // stmdb sp!, {r4-r9, lr}; sub sp, #12; ldr.w r4, [pc, #imm12]
    if (hw[0] != 0xe92d || hw[1] != 0x43f0 || hw[2] != 0xb083 || hw[3] != 0xf8df || (hw[4] & 0xf000) != 0x4000)
        return 0;
    uintptr_t entry = (uintptr_t) hw;
    ff7_framecap_sub_r4 = *(uint32_t *) (((entry + 6 + 4) & ~3u) + (hw[4] & 0xfff));
    ff7_framecap_sub_resume = (entry + 10) | 1;
    return 1;
}

// A logic step, as the original's late path
static void sub_step(uint32_t game) {
    double zero = 0.0, c;
    *(uint32_t *) sub_mem(SUB_FLAG) = 0;
    __builtin_memcpy(sub_mem(SUB_LATE), &zero, 8);
    // [game + 0x40] += 1.0 (a double; its halves may sit on two pages)
    uint32_t * lo = sub_mem(game + 0x40), * hi = sub_mem(game + 0x44), w[2];
    if (lo && hi) {
        __builtin_memcpy(&w[0], lo, 4);
        __builtin_memcpy(&w[1], hi, 4);
        __builtin_memcpy(&c, w, 8);
        c += 1.0;
        __builtin_memcpy(w, &c, 8);
        __builtin_memcpy(lo, &w[0], 4);
        __builtin_memcpy(hi, &w[1], 4);
    }
    sub_steps++;
}

static void sub_limiter(void) {
    volatile uint32_t * regs = ff7_x86_regs();
    if (!cap_on) {
        sub_due = 0;
        sub_original();
        return;
    }
    uint32_t esp = regs[4];
    uint32_t game = *(uint32_t *) sub_mem(esp + 4);
    uint32_t * flag = sub_mem(SUB_FLAG), * catch_up = sub_mem(SUB_CATCH);
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (*flag) {
        // drawn: the next logic step at its time
        port_menu_apply_ab();
        if (sub_due && now < sub_due) {
            SceUInt64 wait = sub_due - now;
            if (wait > FRAME_US) wait = FRAME_US;
            sceKernelDelayThread((SceUInt) wait);
            sub_wait_us += (unsigned) wait;
            now = sceKernelGetProcessTimeWide();
        }
        // a steady 30 fps cadence; after a slow frame, start over from now
        sub_due = sub_due && now - sub_due < FRAME_US ? sub_due + FRAME_US : now + FRAME_US;
        sub_step(game);
        *catch_up = 0;
    } else if (now >= sub_due && !*catch_up) {
        // the next step is already due: one catch-up step before drawing
        sub_due += FRAME_US;
        sub_step(game);
        *catch_up = 1;
        sub_catchups++;
    } else {
        double zero = 0.0;
        *flag = 1;
        *catch_up = 0;
        __builtin_memcpy(sub_mem(SUB_LATE), &zero, 8);
        sub_frames++;
    }
    regs[4] = esp + 4;
}

static void snowboard_frame(void) {
    cap_wait(&caps[0]);
    SO_CONTINUE_HELD(int, caps[0].hook);
}

// the mode's main gets the game object as its argument (cdecl, x86 stack)
static void coaster_frame(void) {
    cap_wait(&caps[1]);
    SO_CONTINUE_HELD(int, caps[1].hook);
}

void ff7_framecap_patch(void) {
    static void (* const natives[NUM_CAPS])(void) = { snowboard_frame, coaster_frame };
    for (int i = 0; i < NUM_CAPS; i++) {
        capped_t * c = &caps[i];
        if (file_exists(c->off_switch)) {
            l_info("ff7_framecap: %s uncapped (%s)", c->name, c->off_switch);
            continue;
        }
        uintptr_t fn = so_symbol(&so_mod, c->sym);
        if (!fn) {
            l_warn("ff7_framecap: %s not found, %s not capped", c->sym, c->name);
            continue;
        }
        c->hook = hook_addr(fn, (uintptr_t) natives[i]);
        l_info("ff7_framecap: %s drawn at 30 fps at most (logic stays 60 Hz)", c->name);
    }

    if (file_exists(SUB_LIMITER_OFF)) {
        l_info("ff7_framecap: submarine keeps its own limiter (%s)", SUB_LIMITER_OFF);
        return;
    }
    uintptr_t fn = so_symbol(&so_mod, "_Z15func_0x0078c9e1v");
    sub_pages = (uint8_t * const *) so_symbol(&so_mod, "MEMMAP");
    if (!fn || !sub_pages || !ff7_x86_regs() || !sub_tramp_setup(fn) || !sub_mem(SUB_FLAG) || !sub_mem(SUB_LATE)
        || !sub_mem(SUB_CATCH)) {
        l_warn("ff7_framecap: unexpected submarine limiter code, left as is");
        return;
    }
    sub_hook = hook_addr(fn, (uintptr_t) sub_limiter);
    l_info("ff7_framecap: submarine limiter 0x78C9E1 replaced by a steady 30 fps cadence");
}

void ff7_framecap_set(int on) {
    if (on != cap_on) l_info("ff7_framecap: 30 fps cap %s", on ? "on" : "off (port menu)");
    cap_on = on;
}

void ff7_framecap_report(double seconds) {
    if (seconds <= 0) return;
    for (int i = 0; i < NUM_CAPS; i++) {
        capped_t * c = &caps[i];
        if (!c->frames) continue;
        l_info("%s cap: %.1f frames/s, %.1f ms/s waiting for 30 fps", c->name, c->frames / seconds,
               c->wait_us / 1000.0 / seconds);
        c->frames = c->wait_us = 0;
    }
    if (sub_frames || sub_steps) {
        l_info("submarine cadence: %.1f frames/s, %.1f logic steps/s (%u catch-up), %.1f ms/s waiting for 30 fps",
               sub_frames / seconds, sub_steps / seconds, sub_catchups, sub_wait_us / 1000.0 / seconds);
        sub_frames = sub_steps = sub_catchups = sub_wait_us = 0;
    }
}
