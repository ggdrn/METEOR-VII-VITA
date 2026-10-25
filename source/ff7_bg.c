/*
 * Native field background renderer for libjni_ff7.so.
 *
 * ff7.exe 0x6465FB turns the field background tiles into textured quads,
 * every frame: for each of the 42 layers (table at *(0xD00050), 0x1804
 * bytes per layer: count, then 0x18-byte tiles {x, y, z, u, v, attr}) it
 * asks 0x66E272 for room in the layer's vertex buffer, then writes four
 * 0x20-byte vertices per tile {x, y, z, 1.0, color, 0xFF000000, u, v} with
 * the tile size and texel offsets added. Recompiled, each tile costs
 * hundreds of emulated instructions and mk_address calls: profiling showed
 * it as the largest single function in busy field scenes.
 *
 * Float math follows the recompiled code: operands widened to double,
 * added in the same order, rounded to float when stored.
 *
 * Self-test: for the first frames the original runs, with 0x66E272 hooked
 * to record what it returned and the buffer state it left; the native code
 * then replays the frame "dry" (writes go to a log, not memory) with those
 * records, and the last value it would have written at every address is
 * compared with memory. On a mismatch the native renderer disables itself.
 *
 * v0.6.9: 0x66E272 is hooked only while the self-test runs. Before, the
 * recorder hook was installed at every boot and, with self-tests off (the
 * default), never removed: every call to 0x66E272 in every mode went through
 * SO_CONTINUE (two rewrites of its entry and two cache flushes), which the
 * minigame profiles showed as 11-19% of the game thread, and which raced the
 * profiler's text permission changes (console reboots with it on).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_bg.h"
#include "ff7_anim.h"
#include "ff7_selftest.h"

#include "utils/logger.h"
#include "utils/profiler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_BG          "_Z15func_0x006465fbv"
#define SYM_ALLOC       "_Z15func_0x0066e272v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define BG_PROLOGUE     0x4ff0e92d

#define SCREEN_PTR   0xdb2bb8
#define OFFSETS      0xd00010 // 16 floats of texel offsets, 0xD00010..0xD0004C
#define LAYER_TABLE  0xd00050
#define LAYER_OBJS   0xcffc70
#define TILE_SIZE    0xcff21c
#define TILE_SCALE   0x7b7918
#define TILE_COLOR   0x906a70
#define ALLOC_RET    0x646805 // return address of the call to 0x66E272
#define LAYERS       42
#define LAYER_STRIDE 0x1804

#define SELFTEST_FRAMES 8
#define MAX_RECORDS     64

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4 };

typedef uint8_t * (* mk_address_fn)(uint32_t addr);
typedef void (* void_fn)(void);

static mk_address_fn mk_address;
static void_fn check_event, alloc_fn;
static volatile uint32_t * regs;
static so_hook bg_hook, alloc_hook;

static int selftest_left = SELFTEST_FRAMES, disabled, alloc_hooked;
static unsigned long long frames, tiles, time_us;

// ---- x86 memory access (4 KB pages need not be contiguous) ----

static inline uint8_t * xp(uint32_t a) {
    return mk_address(a);
}

static inline uint32_t r32(uint32_t a) {
    if ((a & 0xfff) <= 0xffc) return *(uint32_t *) xp(a);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t) *xp(a + i) << (8 * i);
    return v;
}

static inline uint16_t r16(uint32_t a) {
    return (uint16_t) (*xp(a) | (*xp(a + 1) << 8));
}

static inline float f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static inline uint32_t bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float add2(float a, float b) {
    return (float) ((double) a + (double) b);
}

static inline float add3(float a, float b, float c) {
    return (float) (((double) a + (double) b) + (double) c);
}

// ---- writes: to memory, or to the self-test log ----

typedef struct {
    uint32_t addr, val, seq;
    uint8_t size;
} wlog;

static wlog * log_buf;
static uint32_t log_len, log_cap;
static int dry;

static inline void w32(uint32_t a, uint32_t v) {
    if (!dry) {
        if ((a & 0xfff) <= 0xffc) {
            *(uint32_t *) xp(a) = v;
        } else {
            for (int i = 0; i < 4; i++) *xp(a + i) = (uint8_t) (v >> (8 * i));
        }
        return;
    }
    if (log_len == log_cap) {
        log_cap = log_cap ? log_cap * 2 : 65536;
        log_buf = realloc(log_buf, log_cap * sizeof(wlog));
    }
    log_buf[log_len] = (wlog) { a, v, log_len, 4 };
    log_len++;
}

static inline void w8(uint32_t a, uint8_t v) {
    if (!dry) {
        *xp(a) = v;
        return;
    }
    if (log_len == log_cap) {
        log_cap = log_cap ? log_cap * 2 : 65536;
        log_buf = realloc(log_buf, log_cap * sizeof(wlog));
    }
    log_buf[log_len] = (wlog) { a, v, log_len, 1 };
    log_len++;
}

// ---- 0x66E272 records (self-test) ----

typedef struct {
    uint32_t count, buf, ret, b70, b78, b40;
} alloc_rec;

static alloc_rec recs[MAX_RECORDS];
static int num_recs, next_rec, recording, rec_overflow;

static void alloc_recorder(void) {
    uint32_t esp = regs[ESP];
    uint32_t count = r32(esp + 4), buf = r32(esp + 8);
    SO_CONTINUE(int, alloc_hook);
    if (!recording) return;
    if (num_recs == MAX_RECORDS) {
        rec_overflow = 1;
        return;
    }
    recs[num_recs++] = (alloc_rec) { count, buf, regs[EAX], r32(buf + 0x70), r32(buf + 0x78), r32(buf + 0x40) };
}

// Calls the recompiled 0x66E272(count, buf) through the emulated stack
static uint32_t call_alloc(uint32_t count, uint32_t buf, alloc_rec * out) {
    if (dry) {
        if (next_rec >= num_recs) return 0xffffffff; // replay diverged
        alloc_rec * r = &recs[next_rec++];
        if (r->count != count || r->buf != buf) return 0xffffffff;
        *out = *r;
        return r->ret;
    }
    uint32_t esp = regs[ESP];
    uint32_t sp = esp - 12;
    w32(sp + 8, buf);
    w32(sp + 4, count);
    w32(sp, ALLOC_RET);
    regs[ESP] = sp;
    alloc_fn(); // pops the return address
    regs[ESP] += 8; // add esp, 8
    uint32_t ret = regs[EAX];
    out->b70 = r32(buf + 0x70);
    out->b78 = r32(buf + 0x78);
    out->b40 = r32(buf + 0x40);
    return ret;
}

// ---- the renderer ----

// Returns 0, or -1 if a dry replay could not follow the recorded frame
static int bg_render(void) {
    uint32_t screen = r32(SCREEN_PTR);
    if (!dry) check_event(); // end of 0x676578
    uint32_t a = (r32(screen + 0xa90) != 0 && r32(screen + 0xa98) == 0) ? 0xbae00000 : 0;

    // Texel offsets, indexed by (address - 0xD00010) / 4
    uint32_t g[16] = { 0 };
    static const uint8_t set_a[] = { 0x10, 0x14, 0x34, 0x30, 0x20, 0x24, 0x40, 0x48 };
    for (unsigned i = 0; i < sizeof(set_a); i++) g[(set_a[i] - 0x10) / 4] = a;
    for (int i = 0; i < 16; i++) w32(OFFSETS + 4 * i, g[i]);
#define G(off) f32(g[((off) - 0x10) / 4])

    uint32_t table = r32(LAYER_TABLE);
    uint32_t color = r32(TILE_COLOR);
    uint32_t eax = 0, ecx = 0;
    unsigned tile_count = 0;

    for (int L = 0; L < LAYERS; L++) {
        uint32_t lb = table + (uint32_t) L * LAYER_STRIDE;
        int32_t count = (int32_t) r32(lb);
        if (count <= 0) {
            eax = (uint32_t) L * LAYER_STRIDE;
            ecx = table;
            continue;
        }
        uint32_t obj = r32(LAYER_OBJS + 4 * L);
        float s4, s8;
        if (r16(obj + 0x18) == 0) {
            s4 = 0.0625f;
            s8 = f32(r32(TILE_SIZE));
        } else {
            s4 = 0.125f;
            s8 = (float) ((double) f32(r32(TILE_SIZE)) * (double) f32(r32(TILE_SCALE)));
        }
        uint32_t buf = r32(obj + 8);
        alloc_rec st;
        uint32_t ret = call_alloc((uint32_t) count, buf, &st);
        if (ret == 0xffffffff && dry) return -1;
        ecx = table;
        if (ret != 0) {
            int type2 = r16(obj + 0x1a) == 2;
            uint32_t vp = st.b70, bp = st.b78, stride = st.b40;
            int32_t j;
            for (j = 0; j < (int32_t) r32(lb); j++) {
                uint32_t e = lb + (uint32_t) j * 0x18;
                uint32_t xb = r32(e + 4), yb = r32(e + 8), zb = r32(e + 0xc);
                float x = f32(xb), y = f32(yb), u = f32(r32(e + 0x10)), v = f32(r32(e + 0x14));
                uint32_t xs = bits(add2(x, s8)), ys = bits(add2(y, s8));
                uint32_t u0, v0, u1, v1, u2, v2, u3, v3;
                if (!type2) {
                    uint32_t attr = r32(e + 0x18);
                    w8(bp, (uint8_t) attr);
                    w32(buf + 0x7c, attr);
                    u0 = bits(add2(u, G(0x10)));
                    v0 = bits(add2(v, G(0x34)));
                    u1 = bits(add2(u, G(0x14)));
                    v1 = bits(add3(v, G(0x30), s4));
                    u2 = bits(add3(u, s4, G(0x18)));
                    v2 = bits(add2(v, G(0x3c)));
                    u3 = bits(add3(u, G(0x1c), s4));
                    v3 = bits(add3(v, G(0x38), s4));
                } else {
                    u0 = bits(add2(u, G(0x20)));
                    v0 = bits(add2(v, G(0x40)));
                    u1 = bits(add2(u, G(0x24)));
                    v1 = bits(add3(v, s4, G(0x48)));
                    u2 = bits(add3(u, s4, G(0x28)));
                    v2 = bits(add2(v, G(0x44)));
                    u3 = bits(add3(u, s4, G(0x2c)));
                    v3 = bits(add3(v, s4, G(0x4c)));
                }
                const uint32_t px[4] = { xb, xb, xs, xs }, py[4] = { yb, ys, yb, ys };
                const uint32_t pu[4] = { u0, u1, u2, u3 }, pv[4] = { v0, v1, v2, v3 };
                for (int k = 0; k < 4; k++) {
                    uint32_t o = vp + 0x20u * (uint32_t) k;
                    w32(o + 0x00, px[k]);
                    w32(o + 0x04, py[k]);
                    w32(o + 0x08, zb);
                    w32(o + 0x0c, 0x3f800000);
                    w32(o + 0x10, color);
                    w32(o + 0x14, 0xff000000);
                    w32(o + 0x18, pu[k]);
                    w32(o + 0x1c, pv[k]);
                }
                vp += stride;
                w32(buf + 0x70, vp);
                if (!type2) {
                    bp += 1;
                    w32(buf + 0x78, bp);
                }
                tile_count++;
            }
            ecx = type2 ? (uint32_t) j : (uint32_t) L * LAYER_STRIDE;
        }
        w32(obj + 0x14, 1);
        eax = obj;
    }

    if (!dry) {
        regs[EAX] = eax;
        regs[ECX] = ecx;
        regs[EDX] = LAYERS;
        check_event();
        tiles += tile_count;
    }
    return 0;
#undef G
}

static int cmp_log(const void * pa, const void * pb) {
    const wlog * a = pa, * b = pb;
    if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
    return a->seq < b->seq ? -1 : (a->seq > b->seq);
}

// Compares the last logged write at every address with memory
static int verify_log(uint32_t * bad_addr, uint32_t * want, uint32_t * got) {
    qsort(log_buf, log_len, sizeof(wlog), cmp_log);
    for (uint32_t i = 0; i < log_len; i++) {
        if (i + 1 < log_len && log_buf[i + 1].addr == log_buf[i].addr) continue; // not the last write
        wlog * w = &log_buf[i];
        uint32_t m = w->size == 1 ? *xp(w->addr) : r32(w->addr);
        uint32_t expect = w->size == 1 ? (w->val & 0xff) : w->val;
        if (m != expect) {
            *bad_addr = w->addr;
            *want = expect;
            *got = m;
            return 0;
        }
    }
    return 1;
}

static void unhook_alloc(void) {
    if (!alloc_hooked) return;
    profiler_hold();
    sceClibMemcpy((void *) alloc_hook.addr, alloc_hook.orig_instr, sizeof(alloc_hook.orig_instr));
    kuKernelFlushCaches((void *) alloc_hook.addr, sizeof(alloc_hook.orig_instr));
    profiler_release();
    alloc_hooked = 0;
}

volatile uint64_t ff7_bg_last_us;

static void bg_native(void) {
    ff7_bg_last_us = sceKernelGetProcessTimeWide();
    if (disabled) {
        SO_CONTINUE(int, bg_hook);
        return;
    }

    if (selftest_left > 0) {
        // Original frame, recording 0x66E272
        num_recs = next_rec = rec_overflow = 0;
        recording = 1;
        SO_CONTINUE(int, bg_hook);
        recording = 0;

        // Dry replay
        dry = 1;
        log_len = 0;
        int ok = bg_render() == 0 && !rec_overflow && next_rec == num_recs;
        uint32_t addr = 0, want = 0, got = 0;
        if (ok) ok = verify_log(&addr, &want, &got);
        dry = 0;
        if (!ok) {
            l_error("ff7_bg selftest MISMATCH (%u writes, %d/%d layers replayed): at %08x native %08x original %08x"
                    " - native background renderer disabled", log_len, next_rec, num_recs, addr, want, got);
            disabled = 1;
            unhook_alloc();
        } else if (--selftest_left == 0) {
            l_info("ff7_bg selftest ok: %d frames identical (%u writes in the last one)", SELFTEST_FRAMES, log_len);
            unhook_alloc();
            free(log_buf);
            log_buf = NULL;
            log_cap = 0;
        }
        return;
    }

    uint32_t esp = regs[ESP];
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    bg_render();
    regs[ESP] = esp + 4; // ret
    time_us += sceKernelGetProcessTimeWide() - t0;
    frames++;
}

void ff7_bg_patch(void) {
    uintptr_t bg = so_symbol(&so_mod, SYM_BG);
    uintptr_t alloc = so_symbol(&so_mod, SYM_ALLOC);
    check_event = (void_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);
    alloc_fn = (void_fn) alloc;
    regs = ff7_x86_regs();

    if (!bg || !alloc || !check_event || !mk_address || !regs || *(uint32_t *) (bg & ~1) != BG_PROLOGUE) {
        l_warn("ff7_bg: unexpected .so layout, native background renderer disabled");
        return;
    }
    selftest_left = ff7_selftest_count(SELFTEST_FRAMES);
    if (selftest_left > 0) { // the recorder is only for the self-test (see above)
        alloc_hook = hook_addr(alloc, (uintptr_t) &alloc_recorder);
        alloc_hooked = 1;
    }
    bg_hook = hook_addr(bg, (uintptr_t) &bg_native);
    l_info("ff7_bg: native field background renderer installed (selftest on %d frames)", selftest_left);
}

void ff7_bg_report(double seconds) {
    if (seconds <= 0 || !frames) return;
    l_info("field background: %.0f tiles/s native, %.2f ms per frame", tiles / seconds, time_us / 1000.0 / frames);
    frames = tiles = time_us = 0;
}
