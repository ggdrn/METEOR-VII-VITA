/*
 * Native bit reader for battle animation decompression in libjni_ff7.so.
 *
 * ff7.exe's function at 0x5E7C40 reads `n` bits MSB-first from a byte
 * stream and sign-extends them:
 *
 *     int read_bits(const uint8_t * data, int * bit_pos, int n)
 *
 * It is the inner loop of the battle model animation decoder (0x5E7680) and,
 * recompiled to ARM, every bit costs dozens of emulated instructions and
 * several mk_address calls: profiling showed it dominating battle loading.
 *
 * Recompiled x86 functions are `void f(void)` working on a global register
 * file: +0 EAX, +4 ECX, +8 EDX, +16 ESP, +20 EBP, +24 ESI. Callers push the
 * arguments and a return address on the emulated stack; returning pops it
 * (ESP += 4). The replacement keeps the observable effects of the original:
 * EAX/ECX/EDX results, *bit_pos advanced by n, _check_event() once at the end.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_anim.h"
#include "ff7_selftest.h"

#include "utils/logger.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_READ_BITS   "_Z15func_0x005e7c40v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"

// func_0x005e7c40: offset, first instruction, and the literal that leads to
// the register file pointer (ldr r5, [pc, #656] / add r5, pc at +0x0e)
#define READ_BITS_OFFSET   0x17a21c
#define READ_BITS_PROLOGUE 0x43f8e92d // stmdb sp!, {r3-r9, lr}
#define REGS_LITERAL       0x17a4b8
#define REGS_PC            0x17a22e

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4 };

typedef void * (* mk_address_fn)(uint32_t addr);
typedef void (* check_event_fn)(void);

static mk_address_fn mk_address;
static check_event_fn check_event;
static volatile uint32_t * regs;
static so_hook read_bits_hook;
static void decode_patch(void);

#ifdef ENABLE_SELFTESTS
#define SELFTEST_CALLS 2000
#else
#define SELFTEST_CALLS 0
#endif
static int selftest_left = SELFTEST_CALLS;
static unsigned long long calls;

static inline uint32_t stack_arg(uint32_t esp, int index) {
    return *(uint32_t *) mk_address(esp + 4 * index);
}

// Pure computation of the original: value, ECX and EDX it leaves behind
static int32_t read_bits(const uint8_t * data, int32_t pos, int32_t n, uint32_t * ecx, uint32_t * edx) {
    int32_t v = 0;
    for (int32_t i = 0; i < n; i++, pos++) {
        // x86: byte index = pos / 8 rounded toward zero (cdq; and edx,7; add; sar 3)
        int32_t byte = (pos + ((pos >> 31) & 7)) >> 3;
        int bit = 7 - (pos & 7);
        v = (int32_t) ((uint32_t) v << 1);
        if (data[byte] & (1 << bit)) v++;
    }
    uint32_t sh = (uint32_t) (32 - n) & 31; // x86 masks shift counts to 5 bits
    uint32_t shifted = (uint32_t) v << sh;
    *ecx = (uint32_t) (32 - n);
    *edx = shifted;
    return (int32_t) shifted >> sh;
}

static void read_bits_native(void) {
    uint32_t esp = regs[ESP];
    const uint8_t * data = mk_address(stack_arg(esp, 1));
    int32_t * pos = mk_address(stack_arg(esp, 2));
    int32_t n = (int32_t) stack_arg(esp, 3);

    uint32_t ecx, edx;
    int32_t start = *pos;
    int32_t value = read_bits(data, start, n, &ecx, &edx);
    calls++;

    if (selftest_left > 0) {
        // Let the original do the real work and compare
        selftest_left--;
        SO_CONTINUE(int, read_bits_hook);
        if ((int32_t) regs[EAX] != value || *pos != start + (n > 0 ? n : 0)
            || regs[ECX] != ecx || regs[EDX] != edx) {
            l_error("ff7_anim selftest MISMATCH: pos %d n %d: native %d/%u/%u, original %d/%u/%u (pos %d)",
                    start, n, value, ecx, edx, (int32_t) regs[EAX], regs[ECX], regs[EDX], *pos);
            selftest_left = 0;
        } else if (selftest_left == 0) {
            l_info("ff7_anim selftest ok: %d calls identical", SELFTEST_CALLS);
        }
        return;
    }

    if (n > 0) *pos = start + n;
    regs[EAX] = (uint32_t) value;
    regs[ECX] = ecx;
    regs[EDX] = edx;
    check_event();
    regs[ESP] = esp + 4; // ret
}

void ff7_anim_patch(void) {
    uintptr_t base = so_mod.text_base;
    uintptr_t fn = so_symbol(&so_mod, SYM_READ_BITS);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);

    if (!fn || !check_event || !mk_address || (fn & ~1) != base + READ_BITS_OFFSET
        || *(uint32_t *) (base + READ_BITS_OFFSET) != READ_BITS_PROLOGUE) {
        l_warn("ff7_anim: unexpected .so layout, native bit reader disabled");
        return;
    }

    uint32_t literal = *(uint32_t *) (base + REGS_LITERAL);
    regs = *(volatile uint32_t **) (base + REGS_PC + literal);

    read_bits_hook = hook_addr(fn, (uintptr_t) &read_bits_native);
    l_info("ff7_anim: native read_bits installed (x86 registers at %p)", regs);
    decode_patch();
}

/*
 * Native battle animation frame decoder: ff7.exe 0x5E7680 and its helpers
 * 0x5E7B7B (7/16-bit value) and 0x5E7CE4 (delta), all on top of read_bits.
 *
 *     int decode_frame(bone_t * out, int bones, int bit_pos, const uint8_t * src)
 *
 * src: u16 at +2 = data size in bytes, u8 at +4 = rotation shift, bits from
 * +5. bit_pos == 0: key frame (absolute values), else deltas added to `out`.
 * out records are 0x28 bytes: s16 rotation at +0/+2/+4 and the same as
 * 0..4095 ints at +0x10/+0x14/+0x18; record 0 also has the s16 position at
 * +8/+A/+C and as floats at +0x1C/+0x20/+0x24. Returns the new bit position,
 * or 0 at the end of the data.
 *
 * The first calls are checked against the original (same input, memory
 * restored in between); on a mismatch the native decoder disables itself.
 */
#define SYM_DECODE        "_Z15func_0x005e7680v"
#define DECODE_PROLOGUE   0x4df0e92d // stmdb sp!, {r0, r2, r3, r4-r8, sl, fp, lr}
#define DECODE_SELFTESTS  64
#define DECODE_MAX_BONES  256

typedef struct {
    uint32_t page;
    uint8_t * base;
} xcache;

static inline uint8_t * xptr(xcache * c, uint32_t addr) {
    uint32_t page = addr >> 12;
    if (page != c->page) {
        c->page = page;
        c->base = mk_address(addr & ~0xfffu);
    }
    return c->base + (addr & 0xfff);
}

static inline uint8_t xb(xcache * c, uint32_t addr) {
    return *xptr(c, addr);
}

static inline int16_t xr16(uint32_t addr) {
    xcache c = { 0xffffffff, NULL };
    return (int16_t) (xb(&c, addr) | (xb(&c, addr + 1) << 8));
}

static inline void xw16(xcache * c, uint32_t addr, uint16_t v) {
    *xptr(c, addr) = (uint8_t) v;
    *xptr(c, addr + 1) = (uint8_t) (v >> 8);
}

static inline void xw32(xcache * c, uint32_t addr, uint32_t v) {
    for (int i = 0; i < 4; i++) *xptr(c, addr + i) = (uint8_t) (v >> (8 * i));
}

// read_bits on x86 memory: n bits MSB-first from data, sign-extended
static int32_t xread_bits(xcache * c, uint32_t data, int32_t * pos, int32_t n) {
    int32_t v = 0, p = *pos;
    for (int32_t i = 0; i < n; i++, p++) {
        int32_t byte = (p + ((p >> 31) & 7)) >> 3;
        v = (int32_t) ((uint32_t) v << 1);
        if (xb(c, data + (uint32_t) byte) & (1 << (7 - (p & 7)))) v++;
    }
    if (n > 0) *pos = p;
    uint32_t sh = (uint32_t) (32 - n) & 31;
    return (int32_t) ((uint32_t) v << sh) >> sh;
}

// 0x5E7B7B: flag bit, then a signed 7-bit value or a 16-bit value
static uint16_t read_value(xcache * c, uint32_t data, int32_t * pos) {
    int32_t byte = (*pos + ((*pos >> 31) & 7)) >> 3;
    int32_t bit = *pos & 7;
    uint32_t w = ((uint32_t) xb(c, data + byte) << 8) | xb(c, data + byte + 1);
    if (!(w & (1u << (7 - bit + 8)))) {
        *pos += 8;
        return (uint16_t) ((int32_t) (int16_t) (w << (bit + 1)) >> 9);
    }
    w = (w << 8) | xb(c, data + byte + 2);
    *pos += 17;
    return (uint16_t) ((w << (bit + 1)) >> 8);
}

// 0x5E7CE4: optional delta, scaled by `shift`
static uint16_t read_delta(xcache * c, uint32_t data, int32_t * pos, int32_t shift) {
    if (xread_bits(c, data, pos, 1) == 0) return 0;
    int32_t t = xread_bits(c, data, pos, 3) & 7;
    uint32_t sh = (uint32_t) shift & 31;
    if (t == 0) return (uint16_t) (0xffffffffu << sh);
    if (t == 7) return (uint16_t) ((uint32_t) xread_bits(c, data, pos, 12 - shift) << sh);
    int32_t v = xread_bits(c, data, pos, t);
    if (v >= 0) v += 1 << (t - 1);
    else v -= 1 << (t - 1);
    return (uint16_t) ((uint32_t) v << sh);
}

static inline uint32_t float_bits(int16_t v) {
    float f = (float) v; // x87: fild / fdiv 1.0 / fstp, exact for int16
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static int32_t decode_frame(uint32_t out, int32_t bones, int32_t bit_pos, uint32_t src) {
    xcache sc = { 0xffffffff, NULL }, oc = { 0xffffffff, NULL };
    uint16_t total = (uint16_t) (xb(&sc, src + 2) | (xb(&sc, src + 3) << 8));
    int32_t shift = xb(&sc, src + 4);
    uint32_t data = src + 5;
    int32_t pos = bit_pos;

    if (pos == 0) {
        for (int k = 0; k < 3; k++) xw16(&oc, out + 8 + 2 * k, (uint16_t) xread_bits(&sc, data, &pos, 16));
    } else {
        for (int k = 0; k < 3; k++) {
            uint16_t d = read_value(&sc, data, &pos);
            xw16(&oc, out + 8 + 2 * k, (uint16_t) (xr16(out + 8 + 2 * k) + d));
        }
    }
    for (int k = 0; k < 3; k++) xw32(&oc, out + 0x1c + 4 * k, float_bits(xr16(out + 8 + 2 * k)));

    for (int32_t i = 0; i < bones; i++) {
        uint32_t rec = out + (uint32_t) i * 0x28;
        for (int k = 0; k < 3; k++) {
            uint16_t v;
            if (bit_pos == 0) v = (uint16_t) ((uint32_t) xread_bits(&sc, data, &pos, 12 - shift) << (shift & 31));
            else v = (uint16_t) (xr16(rec + 2 * k) + read_delta(&sc, data, &pos, shift));
            xw16(&oc, rec + 2 * k, v);
        }
        for (int k = 0; k < 3; k++) {
            int32_t r = xr16(rec + 2 * k);
            xw32(&oc, rec + 0x10 + 4 * k, (uint32_t) (r < 0 ? r + 0x1000 : r));
        }
    }

    uint32_t bytes = (uint32_t) (((pos + 7) + (((pos + 7) >> 31) & 7)) >> 3) & 0xffff;
    return (int32_t) bytes < (int32_t) total ? pos : 0;
}

static so_hook decode_hook;
static int decode_selftests = DECODE_SELFTESTS, decode_disabled;
static unsigned long long decode_calls, decode_us;

static void decode_native(void) {
    uint32_t esp = regs[ESP];
    uint32_t out = (uint32_t) stack_arg(esp, 1);
    int32_t bones = (int32_t) stack_arg(esp, 2);
    int32_t bit_pos = (int32_t) stack_arg(esp, 3);
    uint32_t src = (uint32_t) stack_arg(esp, 4);

    if (decode_disabled || bones < 0 || bones > DECODE_MAX_BONES) {
        SO_CONTINUE(int, decode_hook);
        return;
    }

    if (decode_selftests > 0) {
        uint32_t size = (uint32_t) (bones > 0 ? bones : 1) * 0x28;
        uint8_t * before = malloc(size), * native = malloc(size);
        xcache c = { 0xffffffff, NULL };
        for (uint32_t i = 0; i < size; i++) before[i] = xb(&c, out + i);
        int32_t ret = decode_frame(out, bones, bit_pos, src);
        for (uint32_t i = 0; i < size; i++) native[i] = xb(&c, out + i);
        for (uint32_t i = 0; i < size; i++) *xptr(&c, out + i) = before[i];

        SO_CONTINUE(int, decode_hook);
        uint32_t diff = size;
        for (uint32_t i = 0; i < size; i++)
            if (xb(&c, out + i) != native[i]) { diff = i; break; }
        if (diff != size || (int32_t) regs[EAX] != ret) {
            l_error("ff7_anim decoder selftest MISMATCH: %d bones, pos %d: byte %u, ret native %d original %d"
                    " - native decoder disabled", bones, bit_pos, diff, ret, (int32_t) regs[EAX]);
            decode_disabled = 1;
        } else if (--decode_selftests == 0) {
            l_info("ff7_anim decoder selftest ok: %d frames identical", DECODE_SELFTESTS);
        }
        free(before);
        free(native);
        return;
    }

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int32_t ret = decode_frame(out, bones, bit_pos, src);
    regs[EAX] = (uint32_t) ret;
    // ECX/EDX as left by the original's tail (size compare)
    xcache sc = { 0xffffffff, NULL };
    regs[ECX] = (uint32_t) (xb(&sc, src + 2) | (xb(&sc, src + 3) << 8));
    regs[EDX] = 0;
    check_event();
    regs[ESP] = esp + 4; // ret
    decode_us += sceKernelGetProcessTimeWide() - t0;
    decode_calls++;
}

static void decode_patch(void) {
    uintptr_t fn = so_symbol(&so_mod, SYM_DECODE);
    if (!fn || !regs || *(uint32_t *) (fn & ~1) != DECODE_PROLOGUE) {
        l_warn("ff7_anim: frame decoder not found, left as is");
        return;
    }
    decode_selftests = ff7_selftest_count(DECODE_SELFTESTS);
    decode_hook = hook_addr(fn, (uintptr_t) &decode_native);
    l_info("ff7_anim: native frame decoder installed (selftest on the first %d frames)", decode_selftests);
}

volatile uint32_t * ff7_x86_regs(void) {
    return regs;
}

void ff7_anim_report(double seconds) {
    if (seconds <= 0) return;
    if (calls) l_info("anim read_bits/s: %.0f", calls / seconds);
    if (decode_calls) l_info("anim frames decoded natively: %llu (%.1f ms)", decode_calls, decode_us / 1000.0);
    calls = 0;
    decode_calls = decode_us = 0;
}
