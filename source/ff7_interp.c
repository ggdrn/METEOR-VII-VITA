/*
 * Battle at 30 fps by interpolation (docs/BATALHA-30FPS.md).
 *
 * Battles run at 15 fps by design (limiter 0x41B965). Every battle frame is
 * shown twice as often here: first an in-between frame with every matched
 * vertex halfway between the previous frame and the new one, 33 ms later
 * the new frame itself. The game's logic, speed and timing are untouched.
 *
 * How a frame goes:
 *  - The game thread draws frame N+1 into its offscreen framebuffer G (640x480
 *    in a 1024x512 texture). gl_queue.c records every record it publishes
 *    (gq_rec_begin) and hands each draw to ff7_interp_draw, which keeps its
 *    x/y/z and u/v with a state key (ff7_interp_core.h).
 *  - The frame ends in SwapBuffers (the GL driver's flip C_006A7157; also
 *    fw_SwapBuffers, the x86 import table's), both just VIDEO_update(), which
 *    draws G's texture on the screen (with ff7_screen.c's zoom, the borders
 *    and the touch UI), hands the frame to the main thread (sem_post) and
 *    waits for its next onDrawFrame (sem_wait). Hooked here:
 *  - N+1 is matched with N (ip_match: same mesh, or triangles by u/v); a cut
 *    (most vertices jumping) or a frame that changed textures or framebuffers
 *    is shown as is. Otherwise its records are replayed (gq_replay) into a
 *    framebuffer of ours (M, sharing G's depth buffer) from the GL state the
 *    frame started with, the positions halfway (ip_mid: a triangle with a
 *    vertex without a pair, or jumping more than 96 px, stays as in N+1).
 *  - VIDEO_update is called with G's texture swapped for M's in its struct:
 *    the main thread shows M. Then, half a battle frame (33.3 ms) after it,
 *    VIDEO_update again with G: the main thread shows N+1.
 *
 * The game thread waits in the hook through those 33 ms (VIDEO_update only
 * returns once the main thread is back), so a frame is interpolated only
 * when its work plus the matching and replay leave time to show the
 * in-between frame half a battle frame after the previous one ("late"
 * otherwise, shown as is). The 15 fps limiter is not changed: a frame that
 * runs past 66.7 ms makes the battle catch up with a second logic step, as
 * it always did.
 *
 * Every 5 s in battle the log shows the frames interpolated and why the
 * others were not, the matching figures (as the phase 0 probe), the time
 * between shown frames and the cost.
 *
 * DATA_PATH "battle_30fps_off.txt" starts with the interpolation off (the
 * matching figures are still logged); the port menu turns it on and off.
 * DATA_PATH "interp_probe_off.txt" (or "natives_off.txt") turns it all off.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_interp.h"
#include "ff7_interp_core.h"
#include "ff7_switch.h"
#include "reimpl/gl_queue.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define VA_MODE     0xcbf9dcu // uint16: 1 field, 2 battle, 3 world map...
#define MODE_BATTLE 2
#define VIDEO_GOT   0xf4c97c  // GOT slot VIDEO_update reads: its framebuffer struct
#define MAX_DRAWS   2048
#define MAX_VERTS   32768
#define KEY_SLOTS   4096      // >= 2 * MAX_DRAWS, power of two
#define TRI_SLOTS   32768     // power of two: draws of up to ~16,000 triangles
#define REC_BYTES   (1024u * 1024u) // one frame's records (a battle frame: ~0.5 MB)
#define GL_FLOAT_   0x1406
#define HALF_US     33333     // half a battle frame
#define MARGIN_US   3000      // composition and swap after the wait
#define OFF_SWITCH  DATA_PATH "battle_30fps_off.txt"

// VIDEO_update's struct (VIDEO_init): framebuffer, texture, depth renderbuffer of
// the offscreen frame, the screen's framebuffer and renderbuffer, texture size
enum { VS_FB = 2, VS_TEX = 3, VS_DEPTH = 4, VS_SCREEN_FB = 5, VS_W = 7, VS_H = 8 };

uint32_t ff7_ip_state[IP_STATE_WORDS];
volatile int ff7_ip_recording;

static uint8_t * (* mk_address)(uint32_t va);
static int (* video_update)(void);
static volatile uint32_t * vs;
static int enabled;
static volatile int interp_on = 1;

static ip_frame frames[2];
static uint8_t * recs[2];
static uint32_t rec_len[2], rec_unsafe[2];
static int cur; // frames[cur] / recs[cur] are being recorded, the other one is the previous frame
static int have_prev;
static ip_scratch scratch;
static int32_t * map;
static void * mem;

static int target_state; // 0 not made yet, 1 ready, -1 failed
static uint32_t m_fb, m_tex;
static SceUInt64 last_shown; // when the last frame was handed to the main thread (0: none in this battle)
static uint32_t est_us = 8000;  // matching + replay, running average

static int expect_screen = 1;
static uint32_t screen_fb = 0xffffffffu;

// Uniform values per program, as the game thread set them (location < 16, program < 16)
static int32_t uni[16][16];

// Figures: the game thread adds to acc; the heartbeat takes them through `taken`
typedef struct {
    ip_stats s;
    uint32_t frames, skipped_draws, other_format;
    uint32_t interp, cut, late, textures, overflow, unsafe, refused, off, first;
    uint64_t match_us, replay_us, gap_m_us, gap_n_us, hook_us, mid_vertices;
    uint32_t gaps_m;
} figures;
static figures acc, taken;
static volatile int take_wanted, take_ready;

static inline SceUInt64 now_us(void) {
    return sceKernelGetProcessTimeWide();
}

static void sleep_until(SceUInt64 t) {
    SceUInt64 n = now_us();
    if (t > n && t - n < 100000) sceKernelDelayThread((SceUInt) (t - n));
}

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
                     int32_t pos_stride, const void * uv, int32_t uv_size, uint32_t uv_type, int32_t uv_stride,
                     int32_t rec) {
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
    ip_add(&frames[cur], hash_words(ff7_ip_state, IP_STATE_WORDS), mode, (uint32_t) count, p, ps, pos_size >= 3, t, us,
           (uint32_t) rec);
}

static int alloc_buffers(void) {
    uint32_t draws = MAX_DRAWS * sizeof(ip_draw), verts = MAX_VERTS * sizeof(ip_vert);
    uint32_t size = 2 * (draws + verts + REC_BYTES) + KEY_SLOTS * sizeof(ip_keyslot) + MAX_DRAWS * sizeof(int32_t)
                    + TRI_SLOTS * sizeof(ip_trislot) + MAX_VERTS * sizeof(int32_t);
    size = (size + 0xfff) & ~0xfffu;
    SceUID id = sceKernelAllocMemBlock("ff7_interp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (id < 0 || sceKernelGetMemBlockBase(id, &mem) < 0) {
        l_warn("ff7_interp: no memory for the battle frames (%u KB, 0x%08x), off", size / 1024, (unsigned) id);
        mem = NULL;
        enabled = 0;
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
        recs[i] = m;
        m += REC_BYTES;
    }
    scratch.keys = (ip_keyslot *) m;
    m += KEY_SLOTS * sizeof(ip_keyslot);
    scratch.next = (int32_t *) m;
    m += MAX_DRAWS * sizeof(int32_t);
    scratch.tris = (ip_trislot *) m;
    m += TRI_SLOTS * sizeof(ip_trislot);
    map = (int32_t *) m;
    scratch.key_size = KEY_SLOTS;
    scratch.tri_size = TRI_SLOTS;
    l_info("ff7_interp: battle frame buffers allocated (%u KB)", size / 1024);
    return 1;
}

static int target_ready(void) {
    if (target_state) return target_state > 0;
    uint32_t w = vs[VS_W], h = vs[VS_H];
    if (w < 64 || w > 4096 || h < 64 || h > 4096 || !vs[VS_FB] || !vs[VS_TEX] || !vs[VS_DEPTH]) {
        l_warn("ff7_interp: unexpected offscreen frame (%ux%u, framebuffer %u, texture %u, depth %u), interpolation off",
               w, h, vs[VS_FB], vs[VS_TEX], vs[VS_DEPTH]);
        target_state = -1;
        return 0;
    }
    int r = gq_make_target(w, h, vs[VS_DEPTH], &m_fb, &m_tex);
    target_state = r > 0 ? 1 : -1;
    if (r > 0)
        l_info("ff7_interp: in-between frame target %ux%u (framebuffer %u, texture %u, depth of the game's %u)", w, h,
               m_fb, m_tex, vs[VS_DEPTH]);
    else
        l_warn("ff7_interp: in-between frame target incomplete (status 0x%x), interpolation off", (unsigned) -r);
    return r > 0;
}

// gq_replay's callback, draws in stream order: halfway positions for the draws of the
// frame being replayed (frames[rb]) that the probe kept, found by their record offset
static int rb;
static uint32_t rd; // next draw of frames[rb]

static void lerp(void * ctx, uint32_t rec, uint8_t * pos, uint32_t stride, int size, uint32_t count, uint32_t mode) {
    (void) ctx;
    const ip_frame * B = &frames[rb];
    while (rd < B->nd && B->d[rd].rec < rec) rd++;
    if (rd >= B->nd || B->d[rd].rec != rec || B->d[rd].count != count || size < 2) return;
    const ip_draw * d = &B->d[rd++];
    acc.mid_vertices += ip_mid(pos, stride, size >= 3, count, mode, map + d->first, frames[!rb].v);
}

static int read_mode(void) {
    uint8_t * pm = mk_address(VA_MODE);
    return pm ? pm[0] | pm[1] << 8 : 0;
}

// fw_SwapBuffers(hdc): VIDEO_update(); return 1. SwapBuffers(hdc) the same
// plus a glGetError, which on the game thread answers GL_NO_ERROR without
// doing anything: left out.
static int swap_native(unsigned arg) {
    (void) arg;
    if (!enabled) {
        video_update();
        return 1;
    }
    SceUInt64 t_in = now_us();
    int mode = read_mode();
    int b = cur, have_b = 0, interp = 0;
    expect_screen = 1;
    if (ff7_ip_recording) {
        ff7_ip_recording = 0;
        rec_len[b] = gq_rec_end(&rec_unsafe[b]);
        have_b = 1;
        acc.frames++;
    }
    if (have_b && !have_prev) {
        acc.first++;
    } else if (have_b) {
        ip_measure = !interp_on;
        SceUInt64 t0 = now_us();
        int cut = ip_match(&frames[!b], &frames[b], &scratch, &acc.s, interp_on ? map : NULL);
        SceUInt64 t1 = now_us();
        acc.match_us += t1 - t0;
        if (!interp_on) acc.off++;
        else if (mode != MODE_BATTLE) acc.first++;
        else if (frames[b].overflow || frames[!b].overflow || (rec_unsafe[b] & GQ_REC_OVERFLOW)) acc.overflow++;
        else if (cut) acc.cut++;
        else if (rec_unsafe[b] & GQ_REC_TEXTURE) acc.textures++;
        else if (rec_unsafe[b]) acc.unsafe++;
        else if (last_shown && t1 + est_us + MARGIN_US > last_shown + HALF_US) acc.late++;
        else if (!target_ready()) acc.refused++;
        else {
            rb = b;
            rd = 0;
            if (gq_replay(recs[b], rec_len[b], b, vs[VS_FB], m_fb, lerp, NULL)) interp = 1;
            else acc.refused++;
            SceUInt64 t2 = now_us();
            acc.replay_us += t2 - t1;
            est_us = (est_us * 7 + (uint32_t) (t2 - t0)) / 8;
        }
    }
    if (interp) {
        if (last_shown) sleep_until(last_shown + HALF_US - MARGIN_US);
        uint32_t g_tex = vs[VS_TEX];
        vs[VS_TEX] = m_tex;
        video_update(); // the main thread shows the in-between frame
        vs[VS_TEX] = g_tex;
        SceUInt64 t_m = now_us();
        if (last_shown) {
            acc.gap_m_us += t_m - last_shown;
            acc.gaps_m++;
        }
        sleep_until(t_m + HALF_US - MARGIN_US);
        video_update(); // and the new frame
        last_shown = now_us();
        acc.gap_n_us += last_shown - t_m;
        acc.interp++;
    } else {
        video_update();
        last_shown = now_us();
    }
    acc.hook_us += last_shown - t_in;

    if (have_b) {
        cur = !cur;
        have_prev = 1;
    }
    if (enabled && mode == MODE_BATTLE && (mem || alloc_buffers())) {
        ip_reset(&frames[cur]);
        gq_rec_begin(recs[cur], REC_BYTES, cur);
        ff7_ip_recording = 1;
    } else {
        have_prev = 0;
        last_shown = 0;
    }
    if (take_wanted && !take_ready) {
        taken = acc;
        memset(&acc, 0, sizeof(acc));
        take_wanted = 0;
        __sync_synchronize();
        take_ready = 1;
    }
    return 1;
}

void ff7_interp_set(int on) {
    interp_on = on != 0;
    l_info("ff7_interp: battle at 30 fps %s", interp_on ? "on" : "off");
}

int ff7_interp_get(void) {
    return enabled && interp_on;
}

int ff7_interp_available(void) {
    return enabled;
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
        l_warn("ff7_interp: unexpected code at SwapBuffers / fw_SwapBuffers, battle at 30 fps off");
        return;
    }
    // VIDEO_update starts with ldr r4, [pc, #748]; add r4, pc; ldr r4, [r4]: this GOT slot
    vs = *(volatile uint32_t **) (so_mod.text_base + VIDEO_GOT);
    uintptr_t v = (uintptr_t) vs;
    if (v < so_mod.text_base || v >= so_mod.text_base + 0x1600000) {
        l_warn("ff7_interp: VIDEO_update's struct not found (%p), battle at 30 fps off", (void *) v);
        return;
    }
    interp_on = !file_exists(OFF_SWITCH);
    enabled = 1;
    hook_addr(fw_swap, (uintptr_t) swap_native);
    hook_addr(swap, (uintptr_t) swap_native);
    l_info("ff7_interp: battle at 30 fps installed (interpolation %s)", interp_on ? "on" : "off by " OFF_SWITCH);
}

void ff7_interp_report(double seconds) {
    (void) seconds;
    if (!enabled) return;
    if (take_ready) {
        __sync_synchronize();
        const figures * f = &taken;
        const ip_stats * s = &f->s;
        if (f->frames) {
            double pairs = s->pairs ? (double) s->pairs : 1.0;
            double verts = s->verts ? (double) s->verts : 1.0;
            l_info("battle 30 fps: %u battle frames, %u interpolated | not: %u cut, %u late, %u textures, %u overflow, "
                   "%u other state, %u not replayable, %u off, %u first | in-between shown %.1f ms after the last "
                   "frame, new frame %.1f ms after it | hook %.1f ms/frame, matching %.2f ms, replay %.2f ms "
                   "(estimate %u us) | %llu vertices moved halfway",
                   f->frames, f->interp, f->cut, f->late, f->textures, f->overflow, f->unsafe, f->refused, f->off,
                   f->first, f->gaps_m ? f->gap_m_us / 1000.0 / f->gaps_m : 0.0,
                   f->interp ? f->gap_n_us / 1000.0 / f->interp : 0.0, f->hook_us / 1000.0 / f->frames,
                   s->pairs ? f->match_us / 1000.0 / s->pairs : 0.0, f->interp ? f->replay_us / 1000.0 / f->interp : 0.0,
                   est_us, (unsigned long long) f->mid_vertices);
            l_info("battle 30 fps matching: draws/frame %.0f, matched %.1f%% | vertices/frame %.0f: same mesh %.1f%%, by "
                   "u/v %.1f%%, unmatched %.1f%% | moving %.1f%% of matched | %u cuts | skipped %u screen draws, %u "
                   "other formats", s->draws / pairs, s->draws ? 100.0 * s->draws_matched / s->draws : 0.0,
                   s->verts / pairs, 100.0 * s->fast / verts, 100.0 * s->by_uv / verts, 100.0 * s->unmatched / verts,
                   s->fast + s->by_uv ? 100.0 * s->moving / (double) (s->fast + s->by_uv) : 0.0, s->cuts,
                   f->skipped_draws, f->other_format);
        }
        take_ready = 0;
    }
    take_wanted = 1;
}
