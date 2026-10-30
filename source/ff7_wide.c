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
 *   - Field background tiles: 0x64085C (native, ff7_fieldbg_core.h) and
 *     0x640C49 both grow their culling window by 38 units per side.
 *   - Field 3D models: 0x639252 skips models outside [0xCFF204].x - 40 ..
 *     + 400 (background units); both grow by 44.
 *   - World map: 0x75F263 (native, ff7_world.c) flags vertices outside the
 *     clip area [0xE2C424] .. + [0xDE68F8]; it grows by 104 on each side.
 *     0x754100's sky plane grows from +/-180 to +/-240 before projection,
 *     keeping its height and colors while covering the wider horizon.
 *     Cloud quads (0x754493) expand horizontally after vertex conversion.
 *   - Full-screen fades and flashes are quads exactly 0 .. 640 wide, of one
 *     color and untextured (field fade, battle fades and flashes, world map
 *     fade): the copy gl_queue makes of each draw is widened to -104 .. 744.
 *   - glReadPixels (battle swirl) reads the central 640 pixels.
 *
 * Not handled (yet): field parallax layers 3/4 and the hand cursor,
 * textured full-screen effects of some summons,
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
#include "ff7_anim.h"

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

static so_hook ortho_hook, cloud_quad_hook, field_layer2_hook;
static volatile uint32_t * regs;
static const uint32_t * memmap;
static const uint32_t * scale; // framebuffer x, y scale
static volatile int want;      // the player's choice
static volatile int active;    // applied (game thread)
static volatile int rendering; // the game's matrix is the wide one
static int field_ok, model_ok, sky_ok, layer2_ok;
static volatile int field_half_width = HALF_WIDTH;
static int applied_half_width = HALF_WIDTH;
static int applied_models;
// 0x754100 builds the sky plane before its primitives are submitted later.
// Retain overscan beyond the edges and grow its half width for 848 pixels.
#define SKY_LEFT_SITE  0x61921c // movw r8, #0xff4c (-180)
#define SKY_RIGHT_SITE 0x619220 // movs r7, #180
#define SKY_HALF_WIDE  240
#define FIELD_EXTRA    38
#define LAYER2_ORIGIN  0x2756a2 // rsb r2, r3, #320
#define LAYER2_LEFT    0x275942 // sub r0, r0, #336

static struct {
    uint32_t widened, outside, checked, clouds;
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

static uint32_t read32(uint32_t va) {
    uint8_t * p = x86(va);
    uint32_t v = 0;
    if (p && (va & 0xfff) <= 0xffc) memcpy(&v, p, 4);
    return v;
}

static void report_projection(float l, float right, float bottom, float top, int wide) {
    static struct { uint32_t l, r, b, t; int mode, wide; } seen[24];
    static unsigned count;
    int mode = game_mode();
    for (unsigned i = 0; i < count; i++)
        if (seen[i].l == bits(l) && seen[i].r == bits(right) && seen[i].b == bits(bottom)
            && seen[i].t == bits(top) && seen[i].mode == mode && seen[i].wide == wide) return;
    if (count == 24) return;
    seen[count++] = (typeof(seen[0])) { bits(l), bits(right), bits(bottom), bits(top), mode, wide };
    l_info("widescreen projection: mode %d, x %.0f..%.0f y %.0f..%.0f -> %s", mode, l, right, top, bottom,
           wide ? "wide" : "original");
}

// 0x754493 converts a cloud FT4 to four 32-byte TL vertices. Its allocator
// leaves the new primitive at buffer+0x70 (the next allocation is +0x80).
// Expand only those vertices, after conversion and before deferred drawing.
static void cloud_quad_hooked(void) {
    uint32_t prim = rendering && game_mode() == MODE_WORLD && regs ? read32(regs[4] + 4) : 0;
    uint32_t buf = prim ? read32(prim + 0x28) : 0;
    uint32_t next = buf ? read32(buf + 0x80) : 0;
    uint32_t count = buf ? read32(buf + 0x4c) : 0;
    uint32_t frame = buf ? read32(buf + 0x54) : 0;
    SO_CONTINUE_HELD(int, cloud_quad_hook);
    if (!buf || (next == read32(buf + 0x80) && count == read32(buf + 0x4c) && frame == read32(buf + 0x54))
        || read32(buf + 0x40) != 0x80) return; // no primitive allocated
    uint32_t vp = read32(buf + 0x70);
    if (!vp) return;
    for (int i = 0; i < 4; i++) if (!x86(vp + 0x20 * i)) return;
    for (int i = 0; i < 4; i++) {
        uint8_t * p = x86(vp + 0x20 * i);
        float x;
        memcpy(&x, p, 4);
        x = 320.0f + (x - 320.0f) * ((GAME_W + 2 * WIDE_EXTRA) / GAME_W);
        memcpy(p, &x, 4);
    }
    st.clouds++;
}

static void write_text(uint32_t off, const uint16_t * hw, int n) {
    void * p = (void *) (so_mod.text_base + off);
    sceClibMemcpy(p, hw, 2 * n);
    kuKernelFlushCaches(p, 2 * n);
}

// The second background layer still runs in the original. Grow its window
// by adjusting x, compensating the origin so its tile positions stay fixed.
static void field_layer2_hooked(void) {
    uint32_t arg = regs ? regs[4] + 4 : 0;
    uint8_t * p = arg ? x86(arg) : NULL;
    uint32_t saved = p ? read32(arg) : 0;
    int grow = active && layer2_ok && p;
    if (grow) {
        uint32_t shifted = (saved & 0xffff0000u) | (uint16_t) ((int16_t) saved + FIELD_EXTRA);
        memcpy(p, &shifted, 4);
    }
    SO_CONTINUE_HELD(int, field_layer2_hook);
    if (grow) memcpy(p, &saved, 4);
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
    if (layer2_ok) {
        // x' = x + E; origin 320+E cancels it; left width 336+2E
        // gives [x-336-E, x+E] while keeping every tile at its original x.
        uint16_t origin[2] = { 0xf5c3, 0x72b3 }; // rsb.w r2, r3, #358
        uint16_t left[2] = { 0xf5a0, 0x70ce };   // sub.w r0, r0, #412
        if (!w) { origin[0] = 0xf5c3; origin[1] = 0x72a0; left[0] = 0xf5a0; left[1] = 0x70a8; }
        profiler_hold();
        write_text(LAYER2_ORIGIN, origin, 2);
        write_text(LAYER2_LEFT, left, 2);
        profiler_release();
    }
    if (sky_ok) {
        // Signed 16-bit plane coordinates, not the projected game pixels.
        int half = w ? SKY_HALF_WIDE : 180;
        uint16_t left[2] = { 0xf64f, (uint16_t) (0x7800 | ((-half) & 0xff)) };
        uint16_t right = (uint16_t) (0x2700 | half);
        profiler_hold();
        write_text(SKY_LEFT_SITE, left, 2);
        write_text(SKY_RIGHT_SITE, &right, 1);
        profiler_release();
    }
    l_info("widescreen: %s", w ? "16:9" : "4:3");
}

/*
 * VIDEO_setOrthoMatrix (left, right, bottom, top, near, far), softfp: floats
 * as raw bits. Every full-width game matrix, including shorter drawing
 * areas and VIDEO_restoreOrthoMatrix, is set to the wide one or back; the
 * viewport the original derives from it would start at x -104, so it is set
 * again from x 0. Menus (including save/load) keep their 640-pixel matrix.
 */
static int ortho_hooked(uint32_t l, uint32_t right_bits, uint32_t b, uint32_t t, uint32_t n, uint32_t f) {
    // SO_CONTINUE declares a local `r`: never pass an argument named `r`
    // through it (its initializer would read that uninitialized local).
    float fl = flt(l), fr = flt(right_bits), fb = flt(b), ft = flt(t);
    int game = ft >= 0.0f && fb > ft && fb <= GAME_H
               && ((fl == 0.0f && fr == GAME_W) || (fl == WIDE_LEFT && fr == WIDE_RIGHT));
    apply();
    int mode = game_mode();
    int w = active && (mode == MODE_FIELD || mode == MODE_BATTLE || mode == MODE_WORLD);
    if (!game) {
        // Local UI windows keep their projection and clipping dimensions.
        // Their viewport lives in the same padded framebuffer as the scene.
        int window = w && fl >= 0 && fr > fl && fr <= GAME_W && ft >= 0 && fb > ft && fb <= GAME_H;
        int result = SO_CONTINUE_HELD(int, ortho_hook, l, right_bits, b, t, n, f);
        if (window) gq_glViewport((int) ((fl + WIDE_EXTRA) * scale[0]), (int) ((GAME_H - fb) * scale[1]),
                                  (int) ((fr - fl) * scale[0]), (int) ((fb - ft) * scale[1]));
        rendering = window;
        report_projection(fl, fr, fb, ft, window);
        return result;
    }
    report_projection(fl, fr, fb, ft, w);
    if (w && mode == MODE_BATTLE && ft == 0 && fb == 332.0f) {
        // Same pixel scale/origin, but let existing scene geometry continue
        // below the command area's upper edge. No geometry means clear black.
        fb = GAME_H;
        b = bits(fb);
    }
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
    return active ? FIELD_EXTRA : 0;
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
    float ymin = GAME_H, ymax = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t * v = pos + (uint32_t) i * pos_stride;
        float x;
        memcpy(&x, v, 4);
        float y;
        memcpy(&y, v + 4, 4);
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
        if (x > -0.5f && x < 0.5f) left++;
        else if (x > GAME_W - 0.5f && x < GAME_W + 0.5f) right++;
        else return;
        if (color && i && memcmp(color, color + (uint32_t) i * color_stride, 4)) return;
        if (uv && i && memcmp(uv, uv + (uint32_t) i * uv_stride, 8)) return;
    }
    if (!left || !right) return;
    // A bottom HUD panel is not a full-screen fade. Keep its 4:3 width.
    if (ymin < -0.5f || ymin > 0.5f || ymax < 400.0f || ymax > GAME_H + 0.5f) return;
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
           "0..640 px (%.0f/s); %.0f cloud quads expanded/s",
           mode, 4 * field_half_width, st.widened / seconds, st.checked ? 100.0 * st.outside / st.checked : 0.0,
           st.outside / seconds, st.clouds / seconds);
    st.widened = st.outside = st.checked = st.clouds = 0;
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
    sky_ok = *(uint16_t *) (base + SKY_LEFT_SITE) == 0xf64f
             && *(uint16_t *) (base + SKY_LEFT_SITE + 2) == 0x784c
             && *(uint16_t *) (base + SKY_RIGHT_SITE) == 0x27b4;
    if (!sky_ok) l_warn("widescreen: world sky code not as expected, sky expansion unavailable");
    uintptr_t cloud_quad = so_symbol(&so_mod, "_Z15func_0x00754493v");
    regs = ff7_x86_regs();
    uintptr_t layer2 = so_symbol(&so_mod, "_Z15func_0x00640c49v");
    layer2_ok = regs && layer2 && *(uint32_t *) (layer2 & ~(uintptr_t) 1) == 0x41f0e92d
                && *(uint32_t *) (base + LAYER2_ORIGIN) == 0x72a0f5c3
                && *(uint32_t *) (base + LAYER2_LEFT) == 0x70a8f5a0;
    if (layer2_ok) {
        field_layer2_hook = hook_addr(layer2, (uintptr_t) &field_layer2_hooked);
        l_info("widescreen: second field tile layer installed");
    } else l_warn("widescreen: unexpected second field layer, its clip window stays original");
    if (cloud_quad && regs && *(uint32_t *) (cloud_quad & ~(uintptr_t) 1) == 0x47f0e92d) {
        cloud_quad_hook = hook_addr(cloud_quad, (uintptr_t) &cloud_quad_hooked);
        l_info("widescreen: cloud quad expansion installed");
    } else l_warn("widescreen: unexpected cloud quad code, cloud expansion unavailable");
    l_info("widescreen: installed (%s; framebuffer scale %ux%u)", want ? "16:9" : "4:3", scale[0], scale[1]);
}
