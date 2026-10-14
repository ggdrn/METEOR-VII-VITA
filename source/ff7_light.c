/*
 * Native per-vertex lighting for libjni_ff7.so (field models).
 *
 * ff7.exe 0x68DD1E lights one vertex:
 *
 *     void light_vertex(const float m[12], const float n[3], const float rgb[3],
 *                       const uint8_t src[4], uint8_t out[4])
 *
 * l = m (3x4) * n, computed by 0x66CFE8 (which the porters rewrote as plain
 * float code: m1*n1 + m2*n2 + m0*n0 + m3 per row). For each channel
 * (out[2] <- rgb[2], l[0]; out[1] <- rgb[1], l[1]; out[0] <- rgb[0], l[2]):
 * if l < ambient (m[3], m[7], m[11]) the value is rgb * ambient, otherwise
 * rgb * l clamped to 255; out[3] = src[3]. Conversions follow the recompiled
 * code (products in double rounded to float, float -> int truncation).
 *
 * It runs for every vertex of every field model: profiling showed it, with
 * the mk_address calls it makes, at about a quarter of the game thread in
 * the slowest field scenes.
 *
 * The first calls are checked against the original (memory restored in
 * between); on a mismatch the native version disables itself.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_light.h"
#include "ff7_light_core.h"
#include "ff7_anim.h"

#include "utils/logger.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_LIGHT       "_Z15func_0x0068dd1ev"
#define SYM_LIGHT2      "_Z15func_0x0068de75v"
#define FPU_GOT         0xf4c82c // GOT slot: pointer to the x87 state
#define FPU_C0          0x5c     // x87 condition flags as the recompiler keeps them
#define FPU_C2          0x5e
#define FPU_C3          0x64
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define SELFTEST_CALLS  2000

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4 };

typedef uint8_t * (* mk_address_fn)(uint32_t addr);
typedef void (* check_event_fn)(void);

static mk_address_fn mk_address;
static check_event_fn check_event;
static volatile uint32_t * regs;
static so_hook light_hook;
static uint32_t prologue;

static int selftest_left = SELFTEST_CALLS, disabled;
static unsigned long long calls;

static so_hook light2_hook;
static uint8_t * fpu;
static int selftest2_left = SELFTEST_CALLS, disabled2;
static unsigned long long calls2;

// Reads n consecutive dwords from x86 memory (handles page crossings)
static inline void xread(uint32_t addr, void * dst, uint32_t n_bytes) {
    uint8_t * d = dst;
    while (n_bytes) {
        uint32_t in_page = 0x1000 - (addr & 0xfff);
        uint32_t n = n_bytes < in_page ? n_bytes : in_page;
        memcpy(d, mk_address(addr), n);
        d += n;
        addr += n;
        n_bytes -= n;
    }
}

static inline uint8_t channel(float rgb, float l, float ambient, int * used_ambient) {
    if (l < ambient) {
        *used_ambient = 1;
        float t = (float) ((double) rgb * (double) ambient);
        return (uint8_t) (int32_t) (double) t;
    }
    *used_ambient = 0;
    float t = (float) ((double) rgb * (double) l);
    if (!((double) t < 255.0)) return 0xff;
    return (uint8_t) (int32_t) (double) t;
}

// Computes the 4 output bytes; returns the EDX the original leaves behind
static uint32_t light(uint32_t m_addr, uint32_t n_addr, uint32_t rgb_addr, uint32_t src_addr, uint8_t out[4]) {
    float m[12], n[3], rgb[3], l[3];
    xread(m_addr, m, sizeof(m));
    xread(n_addr, n, sizeof(n));
    xread(rgb_addr, rgb, sizeof(rgb));
    for (int r = 0; r < 3; r++) {
        const float * row = m + 4 * r;
        float s = row[1] * n[1];
        s = s + row[2] * n[2];
        s = s + row[0] * n[0];
        l[r] = s + row[3];
    }
    int amb;
    out[2] = channel(rgb[2], l[0], m[3], &amb);
    out[1] = channel(rgb[1], l[1], m[7], &amb);
    out[0] = channel(rgb[0], l[2], m[11], &amb);
    out[3] = *mk_address(src_addr + 3);
    return ((amb ? m_addr : rgb_addr) & ~0xffu) | out[3];
}

static inline uint32_t stack_arg(uint32_t esp, int i) {
    uint32_t v;
    xread(esp + 4 * i, &v, 4);
    return v;
}

static void light_native(void) {
    uint32_t esp = regs[ESP];
    uint32_t m = stack_arg(esp, 1), n = stack_arg(esp, 2), rgb = stack_arg(esp, 3);
    uint32_t src = stack_arg(esp, 4), out = stack_arg(esp, 5);

    if (disabled) {
        SO_CONTINUE(int, light_hook);
        return;
    }

    uint8_t res[4];
    uint32_t edx = light(m, n, rgb, src, res);

    if (selftest_left > 0) {
        selftest_left--;
        SO_CONTINUE(int, light_hook);
        uint8_t orig[4];
        xread(out, orig, 4);
        if (memcmp(orig, res, 4)) {
            l_error("ff7_light selftest MISMATCH: native %02x%02x%02x%02x, original %02x%02x%02x%02x"
                    " - native lighting disabled", res[0], res[1], res[2], res[3],
                    orig[0], orig[1], orig[2], orig[3]);
            disabled = 1;
        } else if (selftest_left == 0) {
            l_info("ff7_light selftest ok: %d vertices identical", SELFTEST_CALLS);
        }
        return;
    }

    for (int i = 0; i < 4; i++) *mk_address(out + i) = res[i];
    regs[EAX] = out;
    regs[ECX] = src;
    regs[EDX] = edx;
    check_event(); // 0x66CFE8's
    check_event();
    regs[ESP] = esp + 4; // ret
    calls++;
}

/*
 * 0x68DE75: the same lighting with a per-channel scale and a saturation
 * color (see light_core_vertex2). Profiling showed it, with its mk_address
 * calls, at ~23% of the game thread in the busiest Sector 7 scenes.
 * Checked offline against the recompiled code under emulation (output,
 * EAX/ECX/EDX and x87 flags identical over 24,000 random vertices) and on the
 * first calls in game against the original.
 */
typedef struct {
    uint8_t out[4];
    uint32_t edx;
    int lt, eq;
} light2_result;

static int light2(uint32_t color, uint32_t scale_addr, uint32_t m_addr, uint32_t n_addr, uint32_t rgb_addr,
                  uint32_t src_addr, uint32_t out_addr, light2_result * r) {
    float scale[3], m[12], n[3], rgb[3];
    xread(scale_addr, scale, sizeof(scale));
    xread(m_addr, m, sizeof(m));
    xread(n_addr, n, sizeof(n));
    xread(rgb_addr, rgb, sizeof(rgb));
    int last;
    if (!light_core_vertex2(color, scale, m, n, rgb, *mk_address(src_addr + 3), r->out, &last, &r->lt, &r->eq))
        return 0;
    r->edx = ((last == LIGHT2_NORMAL ? m_addr : out_addr) & ~0xffu) | r->out[3];
    return 1;
}

static void light2_native(void) {
    uint32_t esp = regs[ESP];
    uint32_t color = stack_arg(esp, 1), scale = stack_arg(esp, 2), m = stack_arg(esp, 3), n = stack_arg(esp, 4);
    uint32_t rgb = stack_arg(esp, 5), src = stack_arg(esp, 6), out = stack_arg(esp, 7);

    light2_result r;
    if (disabled2 || !light2(color, scale, m, n, rgb, src, out, &r)) { // NaN: the original
        SO_CONTINUE(int, light2_hook);
        return;
    }

    if (selftest2_left > 0) {
        selftest2_left--;
        SO_CONTINUE(int, light2_hook);
        uint8_t orig[4];
        xread(out, orig, 4);
        if (memcmp(orig, r.out, 4) || regs[EAX] != out || regs[ECX] != src || regs[EDX] != r.edx
            || fpu[FPU_C0] != r.lt || fpu[FPU_C2] != 0 || fpu[FPU_C3] != r.eq) {
            l_error("ff7_light 0x68DE75 selftest MISMATCH: native %02x%02x%02x%02x edx %08x c0 %d c3 %d, original "
                    "%02x%02x%02x%02x edx %08x c0 %d c3 %d - disabled", r.out[0], r.out[1], r.out[2], r.out[3],
                    r.edx, r.lt, r.eq, orig[0], orig[1], orig[2], orig[3], regs[EDX], fpu[FPU_C0], fpu[FPU_C3]);
            disabled2 = 1;
        } else if (selftest2_left == 0) {
            l_info("ff7_light 0x68DE75 selftest ok: %d vertices identical", SELFTEST_CALLS);
        }
        return;
    }

    for (int i = 0; i < 4; i++) *mk_address(out + i) = r.out[i];
    regs[EAX] = out;
    regs[ECX] = src;
    regs[EDX] = r.edx;
    fpu[FPU_C0] = r.lt;
    fpu[FPU_C2] = 0;
    fpu[FPU_C3] = r.eq;
    check_event(); // 0x66CFE8's
    check_event();
    regs[ESP] = esp + 4; // ret
    calls2++;
}

void ff7_light_patch(void) {
    uintptr_t fn = so_symbol(&so_mod, SYM_LIGHT);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);
    regs = ff7_x86_regs();
    if (fn) prologue = *(uint32_t *) (fn & ~1);

    if (!fn || !check_event || !mk_address || !regs) {
        l_warn("ff7_light: unexpected .so layout, native lighting disabled");
        return;
    }
    light_hook = hook_addr(fn, (uintptr_t) &light_native);
    l_info("ff7_light: native vertex lighting installed (prologue %08x, selftest on %d vertices)",
           prologue, SELFTEST_CALLS);

    uintptr_t fn2 = so_symbol(&so_mod, SYM_LIGHT2);
    fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    uintptr_t f = (uintptr_t) fpu;
    if (!fn2 || f < so_mod.text_base || f >= so_mod.text_base + 0x1498e40) {
        l_warn("ff7_light: unexpected .so layout, native 0x68DE75 disabled");
        return;
    }
    light2_hook = hook_addr(fn2, (uintptr_t) &light2_native);
    l_info("ff7_light: native scaled vertex lighting (0x68DE75) installed (selftest on %d vertices)", SELFTEST_CALLS);
}

void ff7_light_report(double seconds) {
    if (seconds <= 0 || (!calls && !calls2)) return;
    l_info("lit vertices/s: %.0f + %.0f scaled (native)", calls / seconds, calls2 / seconds);
    calls = calls2 = 0;
}
