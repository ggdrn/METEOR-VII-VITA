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
 * DATA_PATH "snowboard_60fps.txt" / "coaster_60fps.txt" leave one uncapped.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_framecap.h"
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
}
