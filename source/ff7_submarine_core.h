/*
 * Native functions of the submarine minigame (game mode 10). Shared by
 * ff7_submarine.c and the emulator test (scripts/emu/test_submarine.py,
 * which compiles this header into an ARM blob and runs it next to the
 * recompiled functions). Freestanding; build without -ffast-math and without
 * FP contraction.
 *
 * The sea is a 64 x 64 grid of 16-byte cells at 0xEC7AA8:
 *   +0x0 u16  height now
 *   +0x2 u16  height of the cell above (row - 1), +0x4 left, +0x6 right,
 *             +0x8 below: copies the neighbours keep up to date
 *   +0xA u16  flags: 0x1000 target is +0xC, 0x2000 has particles,
 *             0x8000 timed (low byte: frames left)
 *   +0xC u16  temporary target height, +0xE resting height
 *
 *   0x79F903 ()   cdecl: one step of the sea. Each cell's height goes toward
 *                 its target (+2 / +1 up; down by a quarter of the gap above
 *                 4, else 1) and is copied into its 4 neighbours; a cell at
 *                 its target drops 0x1000; a timed cell counts down and, at
 *                 the end, gets back its resting target
 *   0x7A2EF8 (x)  cdecl: leading bit count of x from byte tables, 8 per
 *                 byte while a byte gives 8 (0xE73F20 for x > 0, 0xE74358
 *                 else; one byte every 4)
 *
 * One _check_event per function at its exit. The final EAX/ECX/EDX are the
 * ones the x86 code leaves; its own stack frame is dead stack after it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SUBMARINE_CORE_H
#define FF7_SUBMARINE_CORE_H

#include <stdint.h>

typedef struct submarine_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    void (* f_66e272)(void);  // vertex buffer room (its x86 entry)
    uint8_t * const * memmap; // mk_address's page table, or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    uint32_t counters[4];     // sea steps, bit counts, splash triangles drawn, water columns drawn
    void (* f_6617e9)(void);  // the camera matrix copy (its x86 entry)
    void (* f_67d2bf)(void);  // the float view matrix (its x86 entry)
    void (* f_66e70a)(void);  // attach a matrix to the deferred texture
    uint32_t surface[4];     // frames, projected quads, deferred quads, culled quads
    // 0x79FB20's matrix set-up (x86 entries): 0x6625AB, 0x6627CD, 0x6626BC
    // (rotations), 0x663673, 0x662035, 0x663707 (matrix stack), 0x662ECC
    // (translation)
    void (* f_model[7])(void);
    uint32_t model[3];       // models, triangles drawn, triangles behind the camera
} submarine_env;

enum { SM_EAX = 0, SM_ECX = 1, SM_EDX = 2, SM_ESP = 4, SM_EBP = 5 };

#define SM_GRID      0xec7aa8u // 64 x 64 cells of 16 bytes
#define SM_GRID_LEN  0x10000u
#define SM_BITS_POS  0xe73f20u
#define SM_BITS_NEG  0xe74358u
#define SM_C0  0x5c
#define SM_C2  0x5e
#define SM_C3  0x64

static inline uint8_t * sm_addr(const submarine_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void sm_event(const submarine_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t sm_rd32(const submarine_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, sm_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *sm_addr(e, va + i) << (8 * i);
    }
    return v;
}

/*
 * The host pointer of [va, va + len) when its pages follow each other in
 * host memory too (one mapping), else NULL: go byte by byte.
 */
static inline uint8_t * sm_span(const submarine_env * e, uint32_t va, uint32_t len) {
    uint8_t * p = sm_addr(e, va);
    for (uint32_t pg = (va & ~0xfffu) + 0x1000; pg < va + len; pg += 0x1000)
        if (sm_addr(e, pg) != p + (pg - va)) return (uint8_t *) 0;
    return p;
}

static inline uint32_t sm_rd16(const submarine_env * e, uint8_t * span, uint32_t va) {
    if (span) {
        uint16_t v;
        __builtin_memcpy(&v, span + (va - SM_GRID), 2);
        return v;
    }
    return *sm_addr(e, va) | (uint32_t) *sm_addr(e, va + 1) << 8;
}

static inline void sm_wr16(const submarine_env * e, uint8_t * span, uint32_t va, uint32_t v) {
    if (span) {
        uint16_t h = (uint16_t) v;
        __builtin_memcpy(span + (va - SM_GRID), &h, 2);
        return;
    }
    *sm_addr(e, va) = (uint8_t) v;
    *sm_addr(e, va + 1) = (uint8_t) (v >> 8);
}

// 0x79F903 (): one step of the sea grid
static inline void sm_sea_step(submarine_env * e) {
    volatile uint32_t * regs = e->regs;
    uint8_t * g = sm_span(e, SM_GRID, SM_GRID_LEN);
    for (uint32_t i = 0; i < 0x1000; i++) {
        uint32_t p = SM_GRID + 16 * i;
        int32_t h = (int32_t) sm_rd16(e, g, p);
        uint32_t flags = sm_rd16(e, g, p + 0xa);
        int32_t tgt = (int32_t) sm_rd16(e, g, p + ((flags & 0x1000) ? 0xc : 0xe));
        if (h != tgt) {
            if (h < tgt) {
                h += tgt - h > 2 ? 2 : 1;
            } else {
                int32_t diff = h - tgt;
                h -= diff > 4 ? diff >> 2 : 1;
            }
            sm_wr16(e, g, p, (uint32_t) h);
            if (i % 64 != 0) sm_wr16(e, g, p - 0x10 + 0x6, (uint32_t) h);
            if (i % 64 != 63) sm_wr16(e, g, p + 0x10 + 0x4, (uint32_t) h);
            if ((i >> 6) != 63) sm_wr16(e, g, p + 0x400 + 0x8, (uint32_t) h);
            if ((i >> 6) != 0) sm_wr16(e, g, p - 0x400 + 0x2, (uint32_t) h);
        } else {
            sm_wr16(e, g, p + 0xa, flags & 0xefff);
        }
        flags = sm_rd16(e, g, p + 0xa);
        if (flags & 0x8000) {
            int32_t lo = (int32_t) (flags & 0xff) - 1;
            uint32_t hi = flags & 0xff00;
            if (lo < 0) {
                lo = 0;
                hi = 0;
                sm_wr16(e, g, p + 0xc, sm_rd16(e, g, p + 0xe));
            }
            sm_wr16(e, g, p + 0xa, hi | (uint32_t) lo);
        }
    }
    regs[SM_EAX] = 0x1000;
    regs[SM_ECX] = SM_GRID + SM_GRID_LEN + 0x400;
    regs[SM_EDX] = SM_GRID + SM_GRID_LEN - 0x400;
    sm_event(e);
    e->counters[0]++;
}

// 0x7A2EF8 (x)'s count and the registers it leaves
static inline uint32_t sm_bits(submarine_env * e, uint32_t x) {
    volatile uint32_t * regs = e->regs;
    int neg = (int32_t) x <= 0;
    uint32_t tbl = neg ? SM_BITS_NEG : SM_BITS_POS, r = 0, v = 0;
    for (int s = 24; s >= 0; s -= 8) {
        v = *sm_addr(e, tbl + 4 * ((x >> s) & 0xff));
        r += v;
        if (v != 8) break;
    }
    regs[SM_EAX] = r;
    regs[SM_ECX] = neg ? v : r;
    regs[SM_EDX] = v;
    sm_event(e);
    e->counters[1]++;
    return r;
}

static inline float sm_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t sm_bits_of(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

static inline void sm_wr32(const submarine_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(sm_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *sm_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline int32_t sm_rds16(const submarine_env * e, uint32_t va) {
    return (int16_t) (*sm_addr(e, va) | *sm_addr(e, va + 1) << 8);
}

// fcomp a against b (doubles); unordered leaves the flags. Returns C0 | C3:
// "test ah, 0x41" taken
static inline int sm_fcomp_le(submarine_env * e, double a, double b) {
    if (!(a != a || b != b)) {
        e->fpu[SM_C0] = a < b;
        e->fpu[SM_C2] = 0;
        e->fpu[SM_C3] = a == b;
    }
    return e->fpu[SM_C0] | e->fpu[SM_C3];
}

// 0x66CE40 (m, in, out): out = in (x, y, z, 1) * m, float, with its _check_event
static inline void sm_transform(const submarine_env * e, const float m[16], const float in[3], float o[4]) {
    for (int j = 0; j < 4; j++) {
        float s = m[4 + j] * in[1];
        s = s + m[8 + j] * in[2];
        s = s + m[j] * in[0];
        o[j] = s + m[12 + j];
    }
    sm_event(e);
}

// An x86 function called from a frame: ESP at its (unwritten) return
// address, EBP the caller's frame; it pops the return address
static inline uint32_t sm_call(submarine_env * e, void (* fn)(void), uint32_t esp_ret, uint32_t ebp) {
    e->regs[SM_ESP] = esp_ret;
    e->regs[SM_EBP] = ebp;
    fn();
    return e->regs[SM_EAX];
}

/*
 * 0x79A61E (), cdecl: the sea splashes. For each splash entry i < [0x988504]
 * (int16 x, y, z, cell at 0xEC0880 + 8 i) on a cell flagged 0x2000 and not
 * 0x8000, each of its 8 triangles j (corner offsets: 3 signed byte indexes
 * of [0x987438 + 4 j] into the int16 table 0x987408; color [0x987458 + 4 j])
 * is drawn when its distance to the camera ([0x987338..40] >> 12, an integer
 * square root by 0x7A2EF8 and the table 0x989CD8) is under [0x9873F8] and
 * the fade-in counter allows it; it goes to texture [0xEC087C] as x/w, y/w,
 * z/w, 1/w, color. When one was drawn, the water column of the entry's
 * cell: 4 walls between the corners of the 64 x 64 float grid 0xEDA688
 * (16-byte points), from y 1023 down to the entry's y rounded to 64, with
 * the corners past the grid's last row/column made from the cell's own,
 * all 16 points in front, to texture [0xEC0878] (24 vertices; x/w, y/w, z/w,
 * w, color). The context 0xD8D488 (0x661000 (0)) gets the parameters; the
 * view matrix comes from 0x66100D, 0x6617E9 and 0x67D2BF as the original
 * calls them. At the end [0xEC1820]++.
 */
#define SM_CTX      0xd8d488u
#define SM_COUNT    0x988504u
#define SM_ENTRIES  0xec0880u
#define SM_TEX_COL  0xec0878u
#define SM_TEX_SPL  0xec087cu
#define SM_FADE     0xec1820u // frame counter, fade-in on, fade-in value
#define SM_COLUMNS  0xeda688u
#define SM_K0       0x7b7fa0u // 0.0 (double)
#define SM_K1       0x7b7fa8u // 1.0f
#define SM_K64      0x7b7facu // 64.0f
#define SM_K32      0x7b7fb0u // 32.0f

static inline int submarine_splash_run(submarine_env * e) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp_in = regs[SM_ESP], ebp_in = regs[SM_EBP];
    uint32_t ebp = esp_in - 4, frame = ebp - 0x3e8;

    // set-up: 0x676578 (), 0x661000 (0), the matrix
    sm_rd32(e, 0xdb2bb8u);
    sm_event(e);
    sm_event(e);
    const uint32_t ctx = SM_CTX;
    uint32_t count = sm_rd32(e, SM_COUNT);
    sm_wr32(e, ctx + 0x30, count);
    sm_wr32(e, ctx + 0x24, SM_ENTRIES);
    sm_wr32(e, ctx + 0x28, 0x987408u);
    sm_wr32(e, ctx + 0x2c, 0x987438u);
    int32_t cam[3];
    for (int k = 0; k < 3; k++) {
        cam[k] = (int32_t) sm_rd32(e, 0x987338u + 4 * k) >> 12;
        sm_wr32(e, ctx + 0x40 + 4 * k, (uint32_t) cam[k]);
    }
    sm_wr32(e, frame - 4, ebp - 0x90);                         // 0x66100D (ebp - 0x90)
    uint32_t cam_m = sm_rd32(e, 0x90aaf0u) + 0x10;
    sm_event(e);
    sm_wr32(e, frame - 8, cam_m);                              // 0x6617E9 (that, ebp - 0x90)
    sm_call(e, e->f_6617e9, frame - 12, ebp);
    sm_wr32(e, frame - 4, ebp - 0x308);                        // 0x67D2BF (ebp - 0x90, ebp - 0x308)
    sm_wr32(e, frame - 8, ebp - 0x90);
    sm_call(e, e->f_67d2bf, frame - 12, ebp);
    float m[16];
    for (int k = 0; k < 16; k++) m[k] = sm_f(sm_rd32(e, ebp - 0x308 + 4 * k));

    int32_t fade_max = (int32_t) sm_rd32(e, 0x9873fcu), dist_max = (int32_t) sm_rd32(e, 0x9873f8u);
    sm_wr32(e, ctx + 0x50, (uint32_t) fade_max);
    sm_wr32(e, ctx + 0x58, (uint32_t) dist_max);
    int32_t fade = 1;
    uint32_t ecx = ctx;
    if (sm_rd32(e, SM_FADE + 4)) {
        fade = (int32_t) (sm_rd32(e, SM_FADE + 8) + sm_rd32(e, 0x987400u));
        if (fade > (int32_t) ((uint32_t) dist_max + (uint32_t) fade_max)) {
            fade = 1;
            sm_wr32(e, SM_FADE + 4, 0);
        }
        sm_wr32(e, SM_FADE + 8, (uint32_t) fade);
        ecx = (uint32_t) fade;
    }
    sm_wr32(e, ctx + 0x4c, (uint32_t) fade);
    int dim = sm_rd32(e, 0x9873a8u) != 0;
    uint32_t frames = sm_rd32(e, SM_FADE);
    uint32_t alpha = dim ? 0x7fu : (0xffu - 4 * frames) & 0xff;
    double k0;
    {
        uint64_t b = (uint64_t) sm_rd32(e, SM_K0) | (uint64_t) sm_rd32(e, SM_K0 + 4) << 32;
        __builtin_memcpy(&k0, &b, 8);
    }
    float k1 = sm_f(sm_rd32(e, SM_K1)), k64 = sm_f(sm_rd32(e, SM_K64)), k32 = sm_f(sm_rd32(e, SM_K32));

    uint32_t i = 0;
    for (; i < count; i++) {
        uint32_t ent = SM_ENTRIES + 8 * i;
        int32_t ex = sm_rds16(e, ent), ey = sm_rds16(e, ent + 2), ez = sm_rds16(e, ent + 4);
        int32_t cell = sm_rds16(e, ent + 6);
        int drawn = 0;
        for (uint32_t j = 0; j < 8; j++) {
            uint32_t offs = sm_rd32(e, 0x987438u + 4 * j);
            uint32_t color = sm_rd32(e, 0x987458u + 4 * j);
            uint32_t flags = sm_rd16(e, (uint8_t *) 0, SM_GRID + ((uint32_t) cell << 4) + 0xa);
            if (!(flags & 0x2000) || (flags & 0x8000)) continue;
            int32_t dx = ex - cam[0], dy = ey - cam[1], dz = ez - cam[2];
            uint32_t sq = (uint32_t) dx * (uint32_t) dx + ((uint32_t) dy * (uint32_t) dy + (uint32_t) dz * (uint32_t) dz);
            int32_t n = (int32_t) sm_bits(e, sq);
            n = (n >> 1) << 1;
            int32_t sh = (31 - n) / 2;
            int32_t v = (int32_t) sq;
            if (n == 32) v = 0;
            if (n < 24) v >>= (24 - n) & 31;
            else v = (int32_t) ((uint32_t) v << ((n - 24) & 31));
            uint32_t root = sm_rd16(e, (uint8_t *) 0, 0x989cd8u + 2 * (uint32_t) v);
            int32_t dist = (int32_t) (root << (sh & 31)) >> 12;
            sm_wr32(e, ctx + 0x54, (uint32_t) dist);
            if (dist >= dist_max) continue;
            int32_t left = (int32_t) ((uint32_t) fade - (uint32_t) dist);
            if (left < 0 || left > fade_max) continue;
            uint32_t col = (color >> 16 & 0xff) | (color >> 8 & 0xff) << 8 | (color & 0xff) << 16 | alpha << 24;

            float in[3][3], o[3][4];
            for (int k = 0; k < 3; k++) {
                int32_t sb = (int8_t) (offs >> (8 * k));
                uint32_t c = 0x987408u + 8 * (uint32_t) sb;
                in[k][0] = (float) (int32_t) ((uint32_t) sm_rds16(e, c) + (uint32_t) ex);
                in[k][1] = (float) (int32_t) ((uint32_t) sm_rds16(e, c + 2) + (uint32_t) ey);
                in[k][2] = (float) (int32_t) ((uint32_t) sm_rds16(e, c + 4) + (uint32_t) ez);
            }
            int behind = 0;
            for (int k = 0; k < 3 && !behind; k++) {
                sm_transform(e, m, in[k], o[k]);
                behind = sm_fcomp_le(e, (double) o[k][3], k0);
            }
            if (behind) continue;
            uint32_t tex = sm_rd32(e, SM_TEX_SPL);
            sm_wr32(e, frame - 4, tex);
            sm_wr32(e, frame - 8, 1);
            if (!sm_call(e, e->f_66e272, frame - 12, ebp)) continue;
            for (int k = 0; k < 3; k++) {
                float inv = (float) ((double) k1 / (double) o[k][3]);
                uint32_t vb = sm_rd32(e, sm_rd32(e, SM_TEX_SPL) + 0x70) + 0x20 * k;
                for (int c = 0; c < 3; c++) sm_wr32(e, vb + 4 * c, sm_bits_of((float) ((double) o[k][c] * (double) inv)));
                sm_wr32(e, vb + 0xc, sm_bits_of(inv));
                sm_wr32(e, vb + 0x10, col);
            }
            e->counters[2]++;
            drawn = 1;
        }
        if (!drawn) continue;

        // the water column: corners A (row, col), B (row + 1, col), C (row + 1, col + 1), D (row, col + 1)
        int32_t gc = cell % 64, gr = (cell - gc) / 64;
        uint32_t base = SM_COLUMNS + ((uint32_t) gr << 10) + ((uint32_t) gc << 4);
        static const uint32_t corner_off[4] = { 0, 0x400, 0x410, 0x10 };
        float cx[4], cz[4];
        for (int k = 0; k < 4; k++) {
            cx[k] = sm_f(sm_rd32(e, base + corner_off[k]));
            cz[k] = sm_f(sm_rd32(e, base + corner_off[k] + 8));
        }
        float d_z = cz[3]; // the third wall's lower D corner keeps the grid's z
        if (gc == 63) {
            float x64 = (float) ((double) cx[0] + (double) k64);
            cx[2] = x64; cz[2] = cz[1];
            cx[3] = x64; cz[3] = cz[0];
        }
        if (gr == 63) {
            float z64 = (float) ((double) cz[3] - (double) k64);
            cx[1] = cx[0]; cz[1] = z64;
            cx[2] = cx[3]; cz[2] = z64;
        }
        float bottom = (float) (int32_t) (((uint32_t) (ey >> 6) << 6) + 64);
        float top = sm_f(0x447fc000u); // 1023.0f
        float p[4][4][4];
        uint32_t pc[4][4];
        int cut = 0;
        for (int w = 0; w < 4 && !cut; w++) {
            int a = w, b = (w + 1) & 3;
            for (int c = 0; c < 4 && !cut; c++) {
                int k = (c & 1) ? b : a;
                float pt[3] = { cx[k], (c & 2) ? bottom : top, (w == 2 && c == 3) ? d_z : cz[k] };
                // The original overwrites C's lower z with A's at the last
                // column; the last-row fix subsequently overwrites it again.
                if (gc == 63 && gr != 63 && w == 2 && c == 2) pt[2] = cz[0];
                pt[0] = (float) ((double) pt[0] - (double) k32);
                pt[2] = (float) ((double) pt[2] + (double) k32);
                sm_transform(e, m, pt, p[w][c]);
                if (sm_fcomp_le(e, (double) p[w][c][3], k0)) {
                    cut = 1;
                    break;
                }
                float inv = (float) ((double) k1 / (double) p[w][c][3]);
                for (int t = 0; t < 3; t++) p[w][c][t] = (float) ((double) p[w][c][t] * (double) inv);
                pc[w][c] = !(c >> 1) ? 0xff7f7fu : dim ? 0x7fff7f7fu : ((0xffu - 4 * frames) << 24) | 0xff7f7fu;
            }
        }
        if (cut || dim) continue;
        uint32_t tex = sm_rd32(e, SM_TEX_COL);
        sm_wr32(e, frame - 4, tex);
        sm_wr32(e, frame - 8, 8);
        if (!sm_call(e, e->f_66e272, frame - 12, ebp)) continue;
        static const uint8_t order[6] = { 0, 1, 2, 2, 1, 3 };
        for (int w = 0; w < 4; w++) {
            for (int k = 0; k < 6; k++) {
                int c = order[k];
                uint32_t vb = sm_rd32(e, sm_rd32(e, SM_TEX_COL) + 0x70) + 0x20 * (6 * w + k);
                for (int t = 0; t < 4; t++) sm_wr32(e, vb + 4 * t, sm_bits_of(p[w][c][t]));
                sm_wr32(e, vb + 0x10, pc[w][c]);
            }
        }
        e->counters[3]++;
    }
    if (count) ecx = i;
    uint32_t next = sm_rd32(e, SM_FADE) + 1;
    sm_wr32(e, SM_FADE, next);
    regs[SM_EAX] = i;
    regs[SM_ECX] = ecx;
    regs[SM_EDX] = next;
    regs[SM_EBP] = ebp_in;
    regs[SM_ESP] = esp_in + 4;
    sm_event(e);
    return 1;
}

// cdecl entries: ESP at the return address, arguments above it
static inline int submarine_sea_run(submarine_env * e) {
    sm_sea_step(e);
    e->regs[SM_ESP] += 4;
    return 1;
}

static inline int submarine_bits_run(submarine_env * e) {
    uint32_t esp = e->regs[SM_ESP];
    sm_bits(e, sm_rd32(e, esp + 4));
    e->regs[SM_ESP] = esp + 4;
    return 1;
}

// 0x7A0190: the 24 x 24 patch of sea around the camera. Debug wireframe
// stays in the original; refusal happens before any observable changes.
static inline int submarine_surface_run(submarine_env * e) {
    uint32_t flags = sm_rd32(e, 0xe7475cu);
    if (flags & 0x40) return 0;
    uint32_t esp = e->regs[SM_ESP], saved_ebp = e->regs[SM_EBP];
    uint32_t ebp = esp - 4, frame = ebp - 0x204;
    uint32_t game = sm_rd32(e, 0xdb2bb8u);
    sm_event(e); // 0x676578
    sm_event(e); // 0x66100D
    sm_wr32(e, frame - 4, ebp - 0x48);
    sm_wr32(e, frame - 8, sm_rd32(e, 0x90aaf0u) + 0x10);
    sm_call(e, e->f_6617e9, frame - 12, ebp);
    sm_wr32(e, frame - 4, ebp - 0x98);
    sm_wr32(e, frame - 8, ebp - 0x48);
    sm_call(e, e->f_67d2bf, frame - 12, ebp);
    for (int k = 0; k < 2; k++) {
        sm_wr32(e, frame - 4, sm_rd32(e, k ? 0xeed690u : 0xeed688u));
        sm_wr32(e, frame - 8, ebp - 0x48);
        sm_wr32(e, frame - 12, 0);
        sm_call(e, e->f_66e70a, frame - 16, ebp);
    }
    float m[16];
    for (int k = 0; k < 16; k++) m[k] = sm_f(sm_rd32(e, ebp - 0x98 + 4 * k));
    int32_t cx = (((int32_t) sm_rd32(e, 0x987338u) >> 12) + 0x800) >> 6;
    int32_t cy = 63 - ((((int32_t) sm_rd32(e, 0x987340u) >> 12) + 0x800) >> 6);
    int32_t left = cx > 12 ? cx - 12 : 0, top = cy > 12 ? cy - 12 : 0;
    int32_t right = left + 24, end = top + 24;
    if (right > 63) { right = 63; left = 39; }
    if (end > 63) { end = 63; top = 39; }
    int flat = (flags & 0x20) != 0;
    uint32_t ecx = e->regs[SM_ECX], edx = e->regs[SM_EDX];
    float near = sm_f(sm_rd32(e, 0x7b7fd4u));
    static const uint32_t offsets[4] = { 0, 16, 1024, 1040 };
    static const uint8_t order[6] = { 0, 1, 2, 2, 1, 3 };
    for (int row = top; row < end; row++) {
        for (int col = left; col < right; col++) {
            uint32_t va = SM_COLUMNS + ((uint32_t) row << 10) + ((uint32_t) col << 4);
            uint32_t colors[4], color = 0xff000000u;
            float in[4][3], out[4][4];
            for (int k = 0; k < 4; k++) {
                uint8_t * point = sm_span(e, va + offsets[k], 16);
                for (int j = 0; j < 3; j++) {
                    uint32_t bits;
                    if (point) __builtin_memcpy(&bits, point + 4 * j, 4);
                    else bits = sm_rd32(e, va + offsets[k] + 4 * j);
                    in[k][j] = sm_f(bits);
                }
                if (point) __builtin_memcpy(&colors[k], point + 12, 4);
                else colors[k] = sm_rd32(e, va + offsets[k] + 12);
                color += colors[k] >> 2;
                sm_transform(e, m, in[k], out[k]);
            }
            int ortho = sm_rd32(e, game + 0xa60) == 1, behind = 0;
            if (!ortho) {
                for (int k = 0; k < 4; k++) {
                    sm_fcomp_le(e, (double) out[k][3], (double) near);
                    if (e->fpu[SM_C0]) { behind++; continue; }
                    float inv = (float) ((double) near / (double) out[k][3]);
                    for (int j = 0; j < 3; j++)
                        out[k][j] = (float) ((double) out[k][j] * (double) inv);
                }
            }
            if (behind == 4 && !ortho) {
                ecx = 4; edx = game;
                e->surface[3]++;
                continue;
            }
            int deferred = behind != 0 || ortho;
            uint32_t tex = sm_rd32(e, deferred ? (flat ? 0xeed690u : 0xeed688u)
                                                    : (flat ? 0xeed68cu : 0xeda680u));
            e->regs[SM_EDX] = ortho ? va + 1040 : 4;
            e->regs[SM_ECX] = deferred ? (ortho ? game : (e->fpu[SM_C0] ? (uint32_t) behind : 48)) : tex;
            sm_wr32(e, frame - 4, tex);
            sm_wr32(e, frame - 8, 2);
            uint32_t ok = sm_call(e, e->f_66e272, frame - 12, ebp);
            ecx = e->regs[SM_ECX]; edx = e->regs[SM_EDX];
            if (!ok) continue;
            for (int k = 0; k < 6; k++) {
                int q = order[k];
                uint32_t vb = sm_rd32(e, sm_rd32(e, deferred ? (flat ? 0xeed690u : 0xeed688u)
                                                                         : (flat ? 0xeed68cu : 0xeda680u)) + 0x70)
                              + 32 * k;
                for (int j = 0; j < (deferred ? 3 : 4); j++)
                    sm_wr32(e, vb + 4 * j, sm_bits_of(deferred ? in[q][j] : out[q][j]));
                sm_wr32(e, vb + 16, flat ? color : colors[q]);
                if (k == 5) {
                    if (deferred) { ecx = sm_bits_of(in[q][2]); edx = vb; }
                    else { ecx = flat ? color : colors[q]; edx = (uint32_t) (col + 1) << 4; }
                }
            }
            e->surface[deferred ? 2 : 1]++;
        }
    }
    e->regs[SM_EAX] = 0;
    e->regs[SM_ECX] = flat ? ecx : (uint32_t) end;
    e->regs[SM_EDX] = flat ? (uint32_t) end : edx;
    e->regs[SM_EBP] = saved_ebp;
    e->regs[SM_ESP] = esp + 4;
    sm_event(e);
    e->surface[0]++;
    return 1;
}

/*
 * 0x79FB20 (mesh), cdecl: draws a submarine model. mesh: +0 int16 x, y, z
 * vertices (8 bytes each), +4 triangles (one u32 each: 3 vertex index bytes),
 * +8 colors (one u32 per corner, 0x00BBGGRR), +0xC triangle count. The
 * model matrix is built from an identity PSX matrix rotated by the angles
 * at 0xEDA678 / 0xEDA67C / 0xEDA67A and the position 0xEDA670 (0x6625AB,
 * 0x6627CD, 0x6626BC, the matrix stack 0xE996F8, 0x662ECC; the translation
 * goes to [0x66100D ()] + 0x12) and turned into the float view matrix by
 * 0x6617E9 / 0x67D2BF, all called as the original does. Each triangle whose
 * 3 corners have w > [0x7B7FC8] (0.0) gets room from 0x66E272 (1, [0xEDA668])
 * and its corners, x/w, y/w, z/w, 1/w, color 0xFFRRGGBB; the color pointer
 * moves 12 bytes per triangle either way.
 */
#define SM_MESH_TEX    0xeda668u
#define SM_MESH_POS    0xeda670u
#define SM_MESH_ANGLES 0xeda678u // 3 int16, used in the order +0, +4, +2
#define SM_MESH_STACK  0xe996f8u
#define SM_MESH_K0     0x7b7fc8u // 0.0 (double)
#define SM_MESH_K1     0x7b7fd0u // 1.0f

enum { SM_ROT_1, SM_ROT_2, SM_ROT_3, SM_PUSH, SM_MUL, SM_POP, SM_APPLY };

static inline int submarine_model_run(submarine_env * e) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp_in = regs[SM_ESP], ebp_in = regs[SM_EBP];
    uint32_t ebp = esp_in - 4, frame = ebp - 0x194;
    uint32_t mesh = sm_rd32(e, esp_in + 4);
    uint32_t verts = sm_rd32(e, mesh), tris = sm_rd32(e, mesh + 4), cols = sm_rd32(e, mesh + 8);
    int32_t count = (int32_t) sm_rd32(e, mesh + 0xc);

    // the model matrix: identity at ebp - 0x170, then the three rotations
    static const uint16_t identity[9] = { 0x1000, 0, 0, 0, 0x1000, 0, 0, 0, 0x1000 };
    for (int k = 0; k < 9; k++) {
        *sm_addr(e, ebp - 0x170 + 2 * k) = (uint8_t) identity[k];
        *sm_addr(e, ebp - 0x170 + 2 * k + 1) = (uint8_t) (identity[k] >> 8);
    }
    static const uint8_t rot[3] = { SM_ROT_1, SM_ROT_2, SM_ROT_3 };
    static const uint8_t angle[3] = { 0, 4, 2 };
    for (int k = 0; k < 3; k++) {
        sm_wr32(e, frame - 4, ebp - 0x170);
        sm_wr32(e, frame - 8, (uint32_t) sm_rds16(e, SM_MESH_ANGLES + angle[k]));
        sm_call(e, e->f_model[rot[k]], frame - 12, ebp);
    }
    sm_wr32(e, frame - 4, SM_MESH_STACK);
    sm_call(e, e->f_model[SM_PUSH], frame - 8, ebp);
    sm_wr32(e, frame - 4, ebp - 0xd8);
    sm_wr32(e, frame - 8, ebp - 0x170);
    sm_wr32(e, frame - 12, SM_MESH_STACK);
    sm_call(e, e->f_model[SM_MUL], frame - 16, ebp);
    sm_wr32(e, frame - 4, SM_MESH_STACK);
    sm_call(e, e->f_model[SM_POP], frame - 8, ebp);
    sm_wr32(e, frame - 4, ebp - 0xb8);
    sm_wr32(e, frame - 8, ebp - 0x18);
    sm_wr32(e, frame - 12, SM_MESH_POS);
    sm_call(e, e->f_model[SM_APPLY], frame - 16, ebp);
    sm_wr32(e, frame - 4, ebp - 0xd8);
    sm_call(e, e->f_model[SM_PUSH], frame - 8, ebp);
    // 0x66100D () gets the translation; 0x66100D (ebp - 0x7C), 0x6617E9, 0x67D2BF the view matrix
    uint32_t cam_m = sm_rd32(e, 0x90aaf0u) + 0x10;
    sm_event(e);
    for (int k = 0; k < 3; k++) sm_wr32(e, cam_m + 0x12 + 4 * k, sm_rd32(e, ebp - 0x18 + 4 * k));
    sm_wr32(e, frame - 4, ebp - 0x7c);
    sm_event(e);
    sm_wr32(e, frame - 8, cam_m);
    sm_call(e, e->f_6617e9, frame - 12, ebp);
    sm_wr32(e, frame - 4, ebp - 0x124);
    sm_wr32(e, frame - 8, ebp - 0x7c);
    sm_call(e, e->f_67d2bf, frame - 12, ebp);
    float m[16];
    for (int k = 0; k < 16; k++) m[k] = sm_f(sm_rd32(e, ebp - 0x124 + 4 * k));

    double k0;
    {
        uint64_t b = (uint64_t) sm_rd32(e, SM_MESH_K0) | (uint64_t) sm_rd32(e, SM_MESH_K0 + 4) << 32;
        __builtin_memcpy(&k0, &b, 8);
    }
    float k1 = sm_f(sm_rd32(e, SM_MESH_K1));
    static const uint8_t out_local[3] = { 0xb0, 0xa0, 0x90 }; // ECX after a corner behind the camera

    int32_t i = 0;
    for (; i < count; i++) {
        uint32_t t = sm_rd32(e, tris + 4 * (uint32_t) i);
        float o[3][4];
        int behind = 0;
        for (int k = 0; k < 3 && !behind; k++) {
            uint32_t v = verts + 8 * ((t >> (8 * k)) & 0xff);
            float in[3] = { (float) sm_rds16(e, v), (float) sm_rds16(e, v + 2), (float) sm_rds16(e, v + 4) };
            sm_transform(e, m, in, o[k]);
            if (sm_fcomp_le(e, (double) o[k][3], k0)) {
                behind = 1;
                regs[SM_ECX] = ebp - out_local[k];
            }
        }
        int drawn = 0;
        if (!behind) {
            uint32_t tex = sm_rd32(e, SM_MESH_TEX);
            sm_wr32(e, frame - 4, tex);
            sm_wr32(e, frame - 8, 1);
            regs[SM_ECX] = tex; // mov ecx, [0xEDA668]; push ecx
            drawn = sm_call(e, e->f_66e272, frame - 12, ebp) != 0;
        } else {
            e->model[2]++;
        }
        if (drawn) {
            uint32_t col = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t c = sm_rd32(e, cols);
                cols += 4;
                col = (c & 0xff) << 16 | (c & 0xff00) | (c >> 16 & 0xff) | 0xff000000u;
                float inv = (float) ((double) k1 / (double) o[k][3]);
                uint32_t vb = sm_rd32(e, sm_rd32(e, SM_MESH_TEX) + 0x70) + 0x20 * k;
                for (int c3 = 0; c3 < 3; c3++)
                    sm_wr32(e, vb + 4 * c3, sm_bits_of((float) ((double) o[k][c3] * (double) inv)));
                sm_wr32(e, vb + 0xc, sm_bits_of(inv));
                sm_wr32(e, vb + 0x10, col);
            }
            regs[SM_ECX] = col;
            e->model[1]++;
        } else {
            cols += 12;
        }
    }
    regs[SM_EAX] = (uint32_t) i;
    if (count > 0) regs[SM_EDX] = (uint32_t) i;
    regs[SM_EBP] = ebp_in;
    regs[SM_ESP] = esp_in + 4;
    sm_event(e);
    e->model[0]++;
    return 1;
}

#endif // FF7_SUBMARINE_CORE_H
