/*
 * Native 16:9 picture without stretching (docs/WIDESCREEN-16x9.md).
 *
 * The game draws in a 640x480 coordinate space into a 1024x512 framebuffer
 * texture: gfx_setviewport ignores the drawing area it is given and calls
 * glOrtho(0, 640, 480, 0), and VIDEO_setOrthoMatrix turns that into the
 * matrix of the shader and a 640x480 glViewport. Vertices arrive already
 * projected to those pixels (2D and the porters' transformed triangles
 * alike), so widening that one matrix to x -104 .. 744 and the viewport to
 * 848 pixels shows whatever the game draws beyond the original edges,
 * centered and at the same scale. 2D windows and menus stay where they were.
 * VIDEO_update then shows a part of the 848 pixels (ff7_screen.c):
 *
 *   field   up to 792 pixels (4 x the half width below), 448 lines
 *   battle  833 pixels, 472 lines (the battle zoom of ff7_screen.c)
 *   world   847 pixels, 480 lines
 *   other   the central 640 (menus, movies, minigames, transitions)
 *
 * What the game itself leaves out beyond 640 pixels:
 *
 *   - Field camera: 0x6438F6 (and 0x643628 for scripted scrolling) clamp the
 *     camera to the background's camera range (trigger header [0xCFF454],
 *     left +0x0C, right +0x10) with a half width of 160. The 24 immediates
 *     160 in the recompiled code become the field's half width: the camera
 *     range / 2, at most 198 (792 pixels). A field narrower than 320 + 2 x 38
 *     shows less (or stays 4:3). Patched on the game thread at the field
 *     background renderer, the frame after the field changes.
 *   - Field background tiles: 0x64085C (native, ff7_fieldbg_core.h) lists
 *     only the tiles inside the window; it grows by the same amount.
 *   - Field 3D models: 0x639252 skips models outside [0xCFF204].x - 40 ..
 *     + 400 (background units); both grow by 44.
 *   - World map: 0x75F263 (native, ff7_world.c) flags vertices outside the
 *     clip area [0xE2C424] .. + [0xDE68F8]; it grows by 104 on each side.
 *   - Full-screen fades and flashes are quads exactly 0 .. 640 wide, of one
 *     color and untextured (field fade, battle fades and flashes, world map
 *     fade): the copy gl_queue makes of each draw is widened to -104 .. 744.
 *   - glReadPixels (battle swirl) reads the central 640 pixels.
 *
 * Not handled (yet): field parallax layers 3/4 and the hand cursor, the
 * world map sky and clouds, textured full-screen effects of some summons,
 * and minigames (left 4:3).
 *
 * The choice comes from the port menu (Gameplay, saved in cheats.txt) and is
 * applied on the game thread at its next drawing area. Every 5 s the log
 * shows what the game drew beyond the 640 pixels, per mode.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_wide.h"
#include "ff7_unhook.h"
#include "ff7_screen.h"
#include "ff7_fieldbg.h"

#include "reimpl/gl_queue.h"
#include "utils/logger.h"
#include "utils/profiler.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_ORTHO        "VIDEO_setOrthoMatrix"
#define ORTHO_PROLOGUE   0xed2db570 // push {r4-r6, lr}; vpush {d8-d9}
#define SCALE_GOT        0xf4c97c   // GOT slot of the framebuffer struct (x, y scale; 1024 x, 512 x)
#define SCALE_GOT_LDR    0x952854   // ldr r3, [pc, #108]; add r3, pc at +6 (VIDEO_setOrthoMatrix)

#define GAME_W           640.0f
#define GAME_H           480.0f
#define WIDE_LEFT        (-(float) WIDE_EXTRA)
#define WIDE_RIGHT       (GAME_W + WIDE_EXTRA)

#define VA_MODE          0xcbf9dcu // uint16: 1 field, 2 battle, 3 world map...
#define VA_TRIGGERS      0xcff454u // -> field trigger header: camera range left +0x0C, right +0x10
#define MODE_FIELD       1
#define MODE_BATTLE      2
#define MODE_WORLD       3

#define HALF_WIDTH       160       // the field camera's half width (background units)
#define HALF_WIDTH_MAX   198       // 792 game pixels: 448 lines fill 960 x 544 at 790.6
static const uint32_t half_width_sites[] = { // adds/subs rN, #160 (Thumb T2)
    0x26e500, 0x26e56e, 0x26e5b0, 0x26e720, // 0x6438F6
    0x26db66, 0x26dbac, 0x26dbd2, 0x26dcaa, 0x26dcce, 0x26dcf4, 0x26dd1a, 0x26ddf6, 0x26de1a, 0x26dee4, // 0x643628
    0x26dfba, 0x26e000, 0x26e026, 0x26e100, 0x26e124, 0x26e14a, 0x26e170, 0x26e24c, 0x26e270, 0x26e33a,
};
#define NUM_HALF_WIDTH_SITES ((int) (sizeof(half_width_sites) / sizeof(half_width_sites[0])))

#define MODEL_LEFT_SITE  0x25beca  // 0x639252: subs r3, #40
#define MODEL_RIGHT_SITE 0x25bf1e  // 0x639252: add.w r3, r3, #400
#define MODEL_LEFT       40
#define MODEL_RIGHT      400
#define MODEL_EXTRA      44        // 38 background units of the widest field, plus a margin

static so_hook ortho_hook;
static const uint32_t * memmap;
static const uint32_t * scale; // framebuffer x, y scale
static volatile int want;      // the player's choice
static volatile int active;    // applied (game thread)
static volatile int rendering; // the game's matrix is the wide one
static int field_ok, model_ok;
static volatile int field_half_width = HALF_WIDTH;
static int applied_half_width = HALF_WIDTH;
static int applied_models;

static struct {
    uint32_t widened, outside, checked;
} st;

static inline uint32_t bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static inline float flt(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static uint8_t * x86(uint32_t va) {
    uint32_t page = memmap ? memmap[va >> 12] : 0;
    return page ? (uint8_t *) (page + (va & 0xfff)) : NULL;
}

static int game_mode(void) {
    uint8_t * p = x86(VA_MODE);
    return p ? *(volatile uint16_t *) p : 0;
}

static void write_text(uint32_t off, const uint16_t * hw, int n) {
    void * p = (void *) (so_mod.text_base + off);
    sceClibMemcpy(p, hw, 2 * n);
    kuKernelFlushCaches(p, 2 * n);
}

static void set_half_width(int hw) {
    profiler_hold();
    for (int i = 0; i < NUM_HALF_WIDTH_SITES; i++) {
        uint16_t v = (uint16_t) ((*(uint16_t *) (so_mod.text_base + half_width_sites[i]) & 0xff00) | hw);
        write_text(half_width_sites[i], &v, 1);
    }
    profiler_release();
    applied_half_width = hw;
    l_info("widescreen: field camera half width %d (%d game pixels)", hw, 4 * hw);
}

static void set_models(int on) {
    int left = MODEL_LEFT + (on ? MODEL_EXTRA : 0), right = MODEL_RIGHT + (on ? MODEL_EXTRA : 0);
    uint16_t l = (uint16_t) (0x3b00 | left);                       // subs r3, #left
    uint16_t r[2] = { 0xf203, (uint16_t) ((right >> 8) << 12 | 3 << 8 | (right & 0xff)) }; // addw r3, r3, #right
    if (!on) { r[0] = 0xf503; r[1] = 0x73c8; }                     // add.w r3, r3, #400
    profiler_hold();
    write_text(MODEL_LEFT_SITE, &l, 1);
    write_text(MODEL_RIGHT_SITE, r, 2);
    profiler_release();
    applied_models = on;
}

// Game thread
static void apply(void) {
    int w = want;
    if (w == active) return;
    active = w;
    if (model_ok && applied_models != w) set_models(w);
    l_info("widescreen: %s", w ? "16:9" : "4:3");
}

/*
 * VIDEO_setOrthoMatrix (left, right, bottom, top, near, far), softfp: floats
 * as raw bits. The game's own matrix (0, 640, 480, 0, or the wide one put
 * back by VIDEO_restoreOrthoMatrix) is set to the wide one or back; the
 * viewport the original derives from it would start at x -104, so it is set
 * again from x 0.
 */
static int ortho_hooked(uint32_t l, uint32_t r, uint32_t b, uint32_t t, uint32_t n, uint32_t f) {
    float fl = flt(l), fr = flt(r), fb = flt(b), ft = flt(t);
    int game = ft == 0.0f && fb == GAME_H
               && ((fl == 0.0f && fr == GAME_W) || (fl == WIDE_LEFT && fr == WIDE_RIGHT));
    if (!game) return SO_CONTINUE_HELD(int, ortho_hook, l, r, b, t, n, f);
    apply();
    int w = active;
    if (w) {
        fl = WIDE_LEFT;
        fr = WIDE_RIGHT;
    } else {
        fl = 0.0f;
        fr = GAME_W;
    }
    int ret = SO_CONTINUE_HELD(int, ortho_hook, bits(fl), bits(fr), b, t, n, f);
    if (w) {
        float sx = (float) scale[0], sy = (float) scale[1];
        gq_glViewport(0, (int) (sy * (GAME_H - fb)), (int) (sx * (fr - fl)), (int) (sy * (fb - ft)));
    }
    rendering = w;
    return ret;
}

void ff7_wide_set(int on) {
    want = on != 0;
}

int ff7_wide_get(void) {
    return want;
}

int ff7_wide_rendering(void) {
    return rendering;
}

float ff7_wide_field_width(void) {
    return 4.0f * (float) field_half_width;
}

int ff7_wide_world_extra(void) {
    return rendering ? WIDE_EXTRA : 0;
}

int ff7_wide_tile_extra(void) {
    return field_half_width - HALF_WIDTH;
}

int ff7_wide_read_offset(void) {
    return rendering && scale ? (int) (WIDE_EXTRA * scale[0]) : 0;
}

void ff7_wide_field_frame(void) {
    if (!field_ok) return;
    int hw = HALF_WIDTH;
    if (active) {
        uint8_t * p = x86(VA_TRIGGERS), * h = NULL;
        uint32_t hdr = p ? *(uint32_t *) p : 0;
        if (hdr && (h = x86(hdr)) && ((hdr + 0x12) & 0xfff) >= 0x12) {
            int16_t left, right;
            memcpy(&left, h + 0x0c, 2);
            memcpy(&right, h + 0x10, 2);
            int half = (right - left) / 2;
            if (half > HALF_WIDTH_MAX) half = HALF_WIDTH_MAX;
            if (half > HALF_WIDTH) hw = half;
        }
    }
    if (hw != applied_half_width) set_half_width(hw);
    field_half_width = hw;
}

/*
 * A full-screen fade or flash: a quad of 4 (strip/fan) or 6 (triangles)
 * vertices whose x are all 0 or 640 (both present), of one color and one
 * texel. Widened to the whole framebuffer.
 */
void ff7_wide_draw(uint32_t mode, int count, uint8_t * pos, uint32_t pos_stride, const uint8_t * color,
                   uint32_t color_stride, const uint8_t * uv, uint32_t uv_stride) {
    (void) mode;
    if (!rendering || !pos) return;
    // What the game draws beyond the 640 pixels (every 8th vertex)
    for (int i = 0; i < count; i += 8) {
        float x;
        memcpy(&x, pos + (uint32_t) i * pos_stride, 4);
        st.checked++;
        if (x < -0.5f || x > GAME_W + 0.5f) st.outside++;
    }
    if (count != 4 && count != 6) return;
    int left = 0, right = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t * v = pos + (uint32_t) i * pos_stride;
        float x;
        memcpy(&x, v, 4);
        if (x > -0.5f && x < 0.5f) left++;
        else if (x > GAME_W - 0.5f && x < GAME_W + 0.5f) right++;
        else return;
        if (color && i && memcmp(color, color + (uint32_t) i * color_stride, 4)) return;
        if (uv && i && memcmp(uv, uv + (uint32_t) i * uv_stride, 8)) return;
    }
    if (!left || !right) return;
    for (int i = 0; i < count; i++) {
        uint8_t * v = pos + (uint32_t) i * pos_stride;
        float x;
        memcpy(&x, v, 4);
        x = x < 1.0f ? WIDE_LEFT : WIDE_RIGHT;
        memcpy(v, &x, 4);
    }
    st.widened++;
}

void ff7_wide_report(double seconds) {
    if (seconds <= 0 || !rendering) return;
    int mode = game_mode();
    l_info("widescreen: mode %d, field %d px; %.0f full-width quads widened/s; %.1f%% of sampled vertices beyond "
           "0..640 px (%.0f/s)",
           mode, 4 * field_half_width, st.widened / seconds, st.checked ? 100.0 * st.outside / st.checked : 0.0,
           st.outside / seconds);
    st.widened = st.outside = st.checked = 0;
}

void ff7_wide_patch(void) {
    uintptr_t base = so_mod.text_base;
    uintptr_t ortho = so_symbol(&so_mod, SYM_ORTHO);
    memmap = (const uint32_t *) so_symbol(&so_mod, "MEMMAP");
    // ldr r3, [pc, #108]; ... add r3, pc: the GOT slot of the framebuffer struct
    uint16_t ldr = *(uint16_t *) (base + SCALE_GOT_LDR), add = *(uint16_t *) (base + SCALE_GOT_LDR + 6);
    uint32_t lit = *(uint32_t *) (base + ((SCALE_GOT_LDR + 4) & ~3u) + 4 * (ldr & 0xff));
    if (!ortho || *(uint32_t *) (ortho & ~1u) != ORTHO_PROLOGUE || !memmap || ldr != 0x4b1b || add != 0x447b
        || SCALE_GOT_LDR + 6 + 4 + lit != SCALE_GOT || !ff7_screen_quad_installed()) {
        l_warn("widescreen: unexpected .so layout, 16:9 unavailable");
        want = 0;
        return;
    }
    scale = *(const uint32_t **) (base + SCALE_GOT);

    // The camera may only show more of a field if the background tile window grows with it, and
    // the field picture is zoomed (it shows 448 lines, the width ff7_screen.c gives it)
    field_ok = ff7_fieldbg_installed() && ff7_screen_zoom_enabled();
    if (!field_ok) l_warn("widescreen: native field background tile lists or field zoom off, fields stay 4:3");
    for (int i = 0; field_ok && i < NUM_HALF_WIDTH_SITES; i++)
        if ((*(uint16_t *) (base + half_width_sites[i]) & 0xf0ff) != 0x30a0) {
            field_ok = 0;
            l_warn("widescreen: field camera code not as expected, fields stay 4:3");
        }
    model_ok = *(uint16_t *) (base + MODEL_LEFT_SITE) == 0x3b28 && *(uint16_t *) (base + MODEL_RIGHT_SITE) == 0xf503
               && *(uint16_t *) (base + MODEL_RIGHT_SITE + 2) == 0x73c8;
    if (!model_ok) l_warn("widescreen: field model culling not as expected, models at the edges may be missing");

    ortho_hook = hook_addr(ortho, (uintptr_t) &ortho_hooked);
    l_info("widescreen: installed (%s; framebuffer scale %ux%u)", want ? "16:9" : "4:3", scale[0], scale[1]);
}
