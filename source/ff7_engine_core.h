/*
 * Native versions of small shared 3D engine conversions of ff7.exe, used by
 * every 3D mode (minigames, battle, world map): PSX fixed point to float.
 *
 *   0x661465 (src, out)  PSX MATRIX (int16 r[9], then int32 t[3] at +0x12,
 *                        packed) to a 4x4 float matrix: r / [0x7B7A50]
 *                        (4096.0) row by row, t through 0x6611DA, last row
 *                        (0, 0, 0, 1)
 *   0x6611DA (x)         (double) x / [0x7B7A54] (1.0), returned on the x87
 *                        stack
 *   0x661098 (src, out)  int16[3] to float[3]
 *
 * Shared by ff7_engine.c and the emulator test (scripts/emu/test_engine.py,
 * which compiles this header into an ARM blob and runs it next to the
 * recompiled functions). Freestanding; build without -ffast-math and
 * without FP contraction. Each follows the recompiled x87 code: divisions
 * in double then stored as float, a division by 0 keeps the dividend and
 * sets ZE, the x86 registers the functions leave, one _check_event per
 * function.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_ENGINE_CORE_H
#define FF7_ENGINE_CORE_H

#include <stdint.h>

typedef struct engine_env {
    uint8_t * (* mk_address)(uint32_t va);
    void (* check_event)(void);
    volatile uint32_t * regs; // x86 registers of the recompiled code
    uint8_t * fpu;            // its x87 state
    uint8_t * const * memmap; // mk_address's page table (x86 page -> host page), or NULL: call it
    uint32_t * ev_count;      // _check_event's call counter, or NULL: call it
    uint32_t counters[4];     // calls of 0x661465, 0x6611DA (alone), 0x661098, 0x66E272 (common path)
} engine_env;

enum { EN_EAX = 0, EN_ECX = 1, EN_EDX = 2, EN_ESP = 4 };

#define EN_ZE  0x56
#define EN_TOP 0x60

// mk_address and _check_event inlined, as in ff7_chocobo_core.h: an unmapped
// page or va 0 go to the real mk_address, every 2048th call to _check_event
// (the one that steps the timers) to the real function.
static inline uint8_t * en_addr(const engine_env * e, uint32_t va) {
    uint8_t * p = e->memmap ? e->memmap[va >> 12] : (uint8_t *) 0;
    return p && va ? p + (va & 0xfff) : e->mk_address(va);
}

static inline void en_event(const engine_env * e) {
    uint32_t c;
    if (e->ev_count && ((c = *e->ev_count + 1) & 0x7ff)) *e->ev_count = c;
    else e->check_event();
}

static inline uint32_t en_rd32(const engine_env * e, uint32_t va) {
    uint32_t v;
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(&v, en_addr(e, va), 4);
    } else {
        v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t) *en_addr(e, va + i) << (8 * i);
    }
    return v;
}

static inline int16_t en_rd16(const engine_env * e, uint32_t va) {
    return (int16_t) (*en_addr(e, va) | (*en_addr(e, va + 1) << 8));
}

static inline void en_wr32(const engine_env * e, uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) {
        __builtin_memcpy(en_addr(e, va), &v, 4);
    } else {
        for (int i = 0; i < 4; i++) *en_addr(e, va + i) = (uint8_t) (v >> (8 * i));
    }
}

static inline float en_f(uint32_t b) {
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

static inline void en_wrf(const engine_env * e, uint32_t va, float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    en_wr32(e, va, b);
}

// fild x; fdiv k: in double, k == 0 keeps x and sets ZE
static inline double en_div(engine_env * e, int32_t x, float k) {
    if (k == 0.0f) {
        e->fpu[EN_ZE] = 1;
        return (double) x;
    }
    return (double) x / (double) k;
}

static inline int en_top_ok(const engine_env * e) {
    uint32_t top;
    __builtin_memcpy(&top, e->fpu + EN_TOP, 4);
    return top <= 7;
}

// 0x661465 (src, out)
static inline int engine_matrix_run(engine_env * e) {
    if (!en_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[EN_ESP];
    uint32_t src = en_rd32(e, esp + 4), out = en_rd32(e, esp + 8);
    float k = en_f(en_rd32(e, 0x7b7a50)), kt = en_f(en_rd32(e, 0x7b7a54));
    static const uint8_t pos[9] = { 0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28 };
    for (int i = 0; i < 9; i++) en_wrf(e, out + pos[i], (float) en_div(e, en_rd16(e, src + 2 * i), k));
    for (int i = 0; i < 3; i++) {
        en_wrf(e, out + 0x0c + 0x10 * i, (float) en_div(e, (int32_t) en_rd32(e, src + 0x12 + 4 * i), kt));
        en_event(e); // 0x6611DA's
    }
    en_wr32(e, out + 0x30, 0);
    en_wr32(e, out + 0x34, 0);
    en_wr32(e, out + 0x38, 0);
    en_wr32(e, out + 0x3c, 0x3f800000u);
    regs[EN_EAX] = out;
    regs[EN_ECX] = out;
    regs[EN_EDX] = out;
    en_event(e);
    regs[EN_ESP] = esp + 4; // ret
    e->counters[0]++;
    return 1;
}

// 0x6611DA (x): pushes the result on the x87 stack
static inline int engine_fixdiv_run(engine_env * e) {
    if (!en_top_ok(e)) return 0;
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[EN_ESP];
    double v = en_div(e, (int32_t) en_rd32(e, esp + 4), en_f(en_rd32(e, 0x7b7a54)));
    uint32_t top;
    __builtin_memcpy(&top, e->fpu + EN_TOP, 4);
    top = (top - 1) & 7;
    __builtin_memcpy(e->fpu + EN_TOP, &top, 4);
    __builtin_memcpy(e->fpu + 8 * top, &v, 8);
    en_event(e);
    regs[EN_ESP] = esp + 4; // ret
    e->counters[1]++;
    return 1;
}

// 0x661098 (src, out)
static inline int engine_vector_run(engine_env * e) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[EN_ESP];
    uint32_t src = en_rd32(e, esp + 4), out = en_rd32(e, esp + 8);
    int16_t z = 0;
    for (int i = 0; i < 3; i++) {
        int16_t c = en_rd16(e, src + 2 * i);
        en_wrf(e, out + 4 * i, (float) c);
        z = c;
    }
    regs[EN_EAX] = src;
    regs[EN_ECX] = (uint32_t) (int32_t) z;
    regs[EN_EDX] = out;
    en_event(e);
    regs[EN_ESP] = esp + 4; // ret
    e->counters[2]++;
    return 1;
}

/*
 * 0x66E272 (n, t): room for n primitives in a texture's vertex buffer t.
 * The common case, done here: t is set, its buffer is current
 * ([t + 0x54] >= [[t + 0x30]], unsigned) and has room (1 <= n <= [t + 0x50],
 * signed). Then the counters move by n and the write pointers
 * [t + 0x70..0x78] get the current position; returns 1. t == 0 returns 0.
 * Any other case (a new frame resets the buffer, or it is full and a new one
 * is allocated) returns -1 with nothing done: the caller runs the original.
 * The x86 registers are the ones the function leaves: EAX = result, and on
 * the common path ECX = the new [p + 0x18] and EDX = t (n == 1) or p (n > 1),
 * p = [t + 0x8c].
 */
// [va, va + len) as one host block if it lies in one page, else NULL (one
// mk_address call instead of one per field)
static inline uint8_t * en_block(const engine_env * e, uint32_t va, uint32_t len) {
    return (va & 0xfff) + len <= 0x1000 ? en_addr(e, va) : (uint8_t *) 0;
}

static inline uint32_t en_get(const engine_env * e, const uint8_t * blk, uint32_t va, uint32_t off) {
    uint32_t v;
    if (blk) {
        __builtin_memcpy(&v, blk + off, 4);
        return v;
    }
    return en_rd32(e, va + off);
}

static inline void en_put(const engine_env * e, uint8_t * blk, uint32_t va, uint32_t off, uint32_t v) {
    if (blk) __builtin_memcpy(blk + off, &v, 4);
    else en_wr32(e, va + off, v);
}

static inline int engine_alloc_run(engine_env * e) {
    volatile uint32_t * regs = e->regs;
    uint32_t esp = regs[EN_ESP];
    const uint8_t * args = en_block(e, esp + 4, 8);
    int32_t n = (int32_t) en_get(e, args, esp + 4, 0);
    uint32_t t = en_get(e, args, esp + 4, 4);
    if (!t) {
        regs[EN_EAX] = 0;
        en_event(e);
        regs[EN_ESP] = esp + 4;
        return 1;
    }
    uint8_t * tb = en_block(e, t, 0x90);
    uint32_t pt = en_get(e, tb, t, 0x30);
    if (en_get(e, tb, t, 0x54) < en_rd32(e, pt) || n < 1 || n > (int32_t) en_get(e, tb, t, 0x50)) return -1;
    uint32_t p = en_get(e, tb, t, 0x8c);
    // p's fields may overlap t's (the writes keep the x86 order, through
    // separate blocks: the same bytes either way)
    uint8_t * pb = en_block(e, p, 0x1c);
    en_put(e, pb, p, 0x8, en_get(e, pb, p, 0x8) + (uint32_t) n);
    en_put(e, tb, t, 0x4c, en_get(e, tb, t, 0x4c) + (uint32_t) n);
    en_put(e, tb, t, 0x50, en_get(e, tb, t, 0x50) - (uint32_t) n);
    en_put(e, tb, t, 0x70, en_get(e, tb, t, 0x80));
    en_put(e, tb, t, 0x74, en_get(e, tb, t, 0x84));
    en_put(e, tb, t, 0x78, en_get(e, tb, t, 0x88));
    uint32_t un = (uint32_t) n;
    en_put(e, tb, t, 0x80, en_get(e, tb, t, 0x80) + en_get(e, tb, t, 0x40) * un);
    en_put(e, tb, t, 0x84, en_get(e, tb, t, 0x84) + en_get(e, tb, t, 0x44) * un);
    en_put(e, tb, t, 0x88, en_get(e, tb, t, 0x88) + un);
    en_put(e, pb, p, 0xc, en_get(e, pb, p, 0xc) + en_get(e, tb, t, 0x38) * un);
    uint32_t v18 = en_get(e, pb, p, 0x18) + en_get(e, tb, t, 0x3c) * un;
    en_put(e, pb, p, 0x18, v18);
    regs[EN_EAX] = 1;
    regs[EN_ECX] = v18;
    regs[EN_EDX] = n == 1 ? t : p;
    en_event(e);
    regs[EN_ESP] = esp + 4;
    e->counters[3]++;
    return 1;
}

#endif // FF7_ENGINE_CORE_H
