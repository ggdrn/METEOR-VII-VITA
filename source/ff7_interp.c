/*
 * Battle at 30 fps, phase 0: draw matching probe (docs/BATALHA-30FPS.md).
 * Nothing on screen changes: this only measures whether consecutive battle
 * frames can be paired draw by draw and vertex by vertex, which an
 * interpolated in-between frame needs.
 *
 * The game thread finishes each frame in SwapBuffers, called by the GL
 * driver's flip C_006A7157 (VIDEO_update: the offscreen framebuffer is drawn
 * to the screen's, then the thread waits for the main thread's next
 * onDrawFrame); fw_SwapBuffers, the same for the x86 import table, is hooked
 * too (v0.6.39 hooked only that one, which battles never call). The entry
 * is hooked: the frame drawn
 * since the previous one is matched with the frame before it
 * (ff7_interp_core.h). While the game is in battle (mode 2), gl_queue.c
 * hands every draw of the game thread to ff7_interp_draw with the vertex
 * arrays still in place; draws to the screen's framebuffer (the composition
 * that follows the hook, the first framebuffer VIDEO_update binds) are left
 * out. The state key of a draw is a hash of the GL state the game thread
 * last set (texture, blend, depth, program, uniforms, matrix, framebuffer...),
 * noted by gl_queue.c.
 *
 * Every 5 s in battle the log shows:
 *   interp probe (battle): frames, draws per frame and matched, vertices per
 *   frame matched in the fast case (same mesh) / by u/v / unmatched, moving
 *   vertices, mean and largest motion in pixels, cuts, cost per frame.
 *
 * Buffers (1.4 MB, a memory block) are allocated on the first battle frame.
 * DATA_PATH "interp_probe_off.txt" (or "natives_off.txt") turns it off.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_interp.h"
#include "ff7_interp_core.h"
#include "ff7_switch.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define VA_MODE     0xcbf9dcu // uint16: 1 field, 2 battle, 3 world map...
#define MODE_BATTLE 2
#define MAX_DRAWS   2048
#define MAX_VERTS   32768
#define KEY_SLOTS   4096      // >= 2 * MAX_DRAWS, power of two
#define TRI_SLOTS   32768     // power of two: draws of up to ~16,000 triangles
#define GL_FLOAT_   0x1406

uint32_t ff7_ip_state[IP_STATE_WORDS];
volatile int ff7_ip_recording;

static uint8_t * (* mk_address)(uint32_t va);
static int (* video_update)(void);
static int enabled;

static ip_frame frames[2];
static int cur; // frames[cur] is being recorded, frames[!cur] is the previous one
static int have_prev;
static ip_scratch scratch;
static void * mem;

static int expect_screen = 1;
static uint32_t screen_fb = 0xffffffffu;

// Uniform values per program, as the game thread set them (location < 16, program < 16)
static int32_t uni[16][16];

// Figures: the game thread adds to acc; the heartbeat takes them through `taken`
static struct {
    ip_stats s;
    uint32_t frames, skipped_draws, other_format;
    uint64_t cost_us;
} acc, taken;
static volatile int take_wanted, take_ready;

static inline uint32_t hash_words(const uint32_t * w, uint32_t n) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++) h = ip_mix(h, w[i]);
    return h;
}

void ff7_ip_note_fb(uint32_t fb) {
    ff7_ip_state[IP_FB] = fb;
    if (expect_screen) { // VIDEO_update's first call: the screen's framebuffer
        screen_fb = fb;
        expect_screen = 0;
    }
}

void ff7_ip_note_texture(uint32_t texture) {
    ff7_ip_state[ff7_ip_state[IP_ACTIVE_TEX] == 0x84c1 ? IP_TEX1 : IP_TEX0] = texture; // GL_TEXTURE1
}

void ff7_ip_note_cap(uint32_t cap, int on) {
    int bit;
    switch (cap) {
        case 0x0be2: bit = 0; break; // GL_BLEND
        case 0x0b71: bit = 1; break; // GL_DEPTH_TEST
        case 0x0b44: bit = 2; break; // GL_CULL_FACE
        case 0x0c11: bit = 3; break; // GL_SCISSOR_TEST
        case 0x0b90: bit = 4; break; // GL_STENCIL_TEST
        case 0x8037: bit = 5; break; // GL_POLYGON_OFFSET_FILL
        default: return;
    }
    if (on) ff7_ip_state[IP_CAPS] |= 1u << bit;
    else ff7_ip_state[IP_CAPS] &= ~(1u << bit);
}

void ff7_ip_note_program(uint32_t program) {
    ff7_ip_state[IP_PROGRAM] = program;
    ff7_ip_state[IP_UNIFORMS] = program < 16 ? hash_words((const uint32_t *) uni[program], 16) : 0;
}

void ff7_ip_note_uniform(int32_t location, int32_t value) {
    uint32_t p = ff7_ip_state[IP_PROGRAM];
    if (p >= 16 || location < 0 || location >= 16) return;
    uni[p][location] = value;
    ff7_ip_state[IP_UNIFORMS] = hash_words((const uint32_t *) uni[p], 16);
}

void ff7_ip_note_words(int word, const void * data, uint32_t len) {
    uint32_t w[16] = {0};
    memcpy(w, data, len < sizeof(w) ? len : sizeof(w));
    ff7_ip_state[word] = hash_words(w, (len < sizeof(w) ? len : sizeof(w)) / 4);
}

void ff7_ip_note_viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
    const int32_t v[4] = { x, y, w, h };
    ff7_ip_state[IP_VIEWPORT] = hash_words((const uint32_t *) v, 4);
}

void ff7_interp_draw(uint32_t mode, int32_t first, int32_t count, const void * pos, int32_t pos_size, uint32_t pos_type,
                     int32_t pos_stride, const void * uv, int32_t uv_size, uint32_t uv_type, int32_t uv_stride) {
    if (count <= 0) return;
    if (ff7_ip_state[IP_FB] == screen_fb) {
        acc.skipped_draws++;
        return;
    }
    if (!pos || pos_type != GL_FLOAT_ || pos_size < 2) {
        acc.other_format++;
        return;
    }
    uint32_t ps = pos_stride ? (uint32_t) pos_stride : (uint32_t) pos_size * 4;
    const uint8_t * p = (const uint8_t *) pos + (uint32_t) first * ps;
    const uint8_t * t = NULL;
    uint32_t us = 0;
    if (uv && uv_type == GL_FLOAT_ && uv_size >= 2) {
        us = uv_stride ? (uint32_t) uv_stride : (uint32_t) uv_size * 4;
        t = (const uint8_t *) uv + (uint32_t) first * us;
    }
    ip_add(&frames[cur], hash_words(ff7_ip_state, IP_STATE_WORDS), mode, (uint32_t) count, p, ps, t, us);
}

static int alloc_buffers(void) {
    uint32_t draws = MAX_DRAWS * sizeof(ip_draw), verts = MAX_VERTS * sizeof(ip_vert);
    uint32_t size = 2 * (draws + verts) + KEY_SLOTS * sizeof(ip_keyslot) + MAX_DRAWS * sizeof(int32_t)
                    + TRI_SLOTS * sizeof(ip_trislot);
    size = (size + 0xfff) & ~0xfffu;
    SceUID id = sceKernelAllocMemBlock("ff7_interp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (id < 0 || sceKernelGetMemBlockBase(id, &mem) < 0) {
        l_warn("ff7_interp: no memory for the probe (%u KB, 0x%08x), off", size / 1024, (unsigned) id);
        return 0;
    }
    uint8_t * m = mem;
    for (int i = 0; i < 2; i++) {
        frames[i].d = (ip_draw *) m;
        m += draws;
        frames[i].v = (ip_vert *) m;
        m += verts;
        frames[i].maxd = MAX_DRAWS;
        frames[i].maxv = MAX_VERTS;
    }
    scratch.keys = (ip_keyslot *) m;
    m += KEY_SLOTS * sizeof(ip_keyslot);
    scratch.next = (int32_t *) m;
    m += MAX_DRAWS * sizeof(int32_t);
    scratch.tris = (ip_trislot *) m;
    scratch.key_size = KEY_SLOTS;
    scratch.tri_size = TRI_SLOTS;
    l_info("ff7_interp: probe buffers allocated (%u KB)", size / 1024);
    return 1;
}

// Game thread, at the end of every frame
static void frame_end(void) {
    uint8_t * pm = mk_address(VA_MODE);
    int mode = pm ? pm[0] | pm[1] << 8 : 0;
    if (ff7_ip_recording) {
        if (have_prev) {
            SceUInt64 t0 = sceKernelGetProcessTimeWide();
            ip_match(&frames[!cur], &frames[cur], &scratch, &acc.s);
            acc.cost_us += sceKernelGetProcessTimeWide() - t0;
        }
        acc.frames++;
        cur = !cur;
        have_prev = 1;
    }
    if (mode == MODE_BATTLE && (mem || alloc_buffers())) {
        ip_reset(&frames[cur]);
        ff7_ip_recording = 1;
    } else {
        ff7_ip_recording = 0;
        have_prev = 0;
    }
    expect_screen = 1;
    if (take_wanted && !take_ready) {
        taken = acc;
        memset(&acc, 0, sizeof(acc));
        take_wanted = 0;
        __sync_synchronize();
        take_ready = 1;
    }
}

// fw_SwapBuffers(hdc): VIDEO_update(); return 1. SwapBuffers(hdc) the same
// plus a glGetError, which on the game thread answers GL_NO_ERROR without
// doing anything: left out.
static int swap_native(unsigned arg) {
    (void) arg;
    if (enabled) frame_end();
    video_update();
    return 1;
}

void ff7_interp_patch(void) {
    if (ff7_switch_off("ff7_interp", DATA_PATH "interp_probe_off.txt")) return;
    uintptr_t fw_swap = so_symbol(&so_mod, "_Z14fw_SwapBuffersj"), swap = so_symbol(&so_mod, "SwapBuffers");
    video_update = (int (*)(void)) so_symbol(&so_mod, "VIDEO_update");
    mk_address = (uint8_t * (*)(uint32_t)) so_symbol(&so_mod, "mk_address");
    // push {r3, lr}; bl VIDEO_update; movs r0, #1; pop {r3, pc}: 12 bytes, the hook fits
    static const uint16_t fw_code[] = { 0xb508, 0xf1c4, 0xf857, 0x2001, 0xbd08 };
    // push {r3, lr}; bl VIDEO_update; blx glGetError; movs r0, #1; pop {r3, pc}: 16 bytes
    static const uint16_t code[] = { 0xb508, 0xf7ff, 0xfd63, 0xf75b, 0xcb4a, 0x2001, 0xbd08 };
    if (!fw_swap || !swap || !video_update || !mk_address || memcmp((void *) (fw_swap & ~1u), fw_code, sizeof(fw_code))
        || memcmp((void *) (swap & ~1u), code, sizeof(code))) {
        l_warn("ff7_interp: unexpected code at SwapBuffers / fw_SwapBuffers, battle probe off");
        return;
    }
    enabled = 1;
    hook_addr(fw_swap, (uintptr_t) swap_native);
    hook_addr(swap, (uintptr_t) swap_native);
    l_info("ff7_interp: battle 30 fps probe installed (matching consecutive battle frames, nothing drawn)");
}

void ff7_interp_report(double seconds) {
    (void) seconds;
    if (!enabled) return;
    if (take_ready) {
        __sync_synchronize();
        const ip_stats * s = &taken.s;
        if (taken.frames) {
            double pairs = s->pairs ? (double) s->pairs : 1.0;
            double verts = s->verts ? (double) s->verts : 1.0;
            l_info("interp probe (battle): %u frames (%u matched pairs, %u cuts, %u overflows) | draws/frame %.0f, "
                   "matched %.1f%% (%llu triangle lists, %llu other) | vertices/frame %.0f: same mesh %.1f%%, by u/v "
                   "%.1f%%, unmatched %.1f%% | moving %.1f%% of matched, mean %.2f px, max %.1f px, jumps > %.0f px "
                   "%llu | skipped %u screen draws, %u other formats (screen fb %u) | cost %.2f ms/frame",
                   taken.frames, s->pairs, s->cuts, s->overflows, s->draws / pairs,
                   s->draws ? 100.0 * s->draws_matched / s->draws : 0.0, (unsigned long long) s->draws_tri,
                   (unsigned long long) s->draws_other, s->verts / pairs, 100.0 * s->fast / verts,
                   100.0 * s->by_uv / verts, 100.0 * s->unmatched / verts,
                   s->fast + s->by_uv ? 100.0 * s->moving / (double) (s->fast + s->by_uv) : 0.0,
                   s->moving ? s->dsum / s->moving : 0.0, s->dmax, IP_CUT_PX, (unsigned long long) s->far,
                   taken.skipped_draws, taken.other_format, screen_fb,
                   s->pairs ? taken.cost_us / 1000.0 / s->pairs : 0.0);
        }
        take_ready = 0;
    }
    take_wanted = 1;
}
