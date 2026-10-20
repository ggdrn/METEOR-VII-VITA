/*
 * Native world map vertex projection for libjni_ff7.so.
 *
 * ff7.exe draws the world map terrain mesh by mesh (0x751EFC). For each mesh
 * 0x75F263 transforms a fixed set of 126 vertices (in[] at 0xE38338, 12 bytes
 * each) by the camera matrix at 0xE39738 into out[] (0xE38F38, x y z w),
 * projects the first 123 (x / w, y / w into 0xE38938) and sets a clip flag
 * word for each (0xE397B8), which 0x75F68C then reads triangle by triangle.
 * In the recompiled code each vertex costs a call to 0x66CE40, two to
 * 0x75F090 and dozens of mk_address calls through the emulated x87 stack:
 * profiling the world map (v0.5.05, profile_all.txt) put it at ~12% of the
 * game thread. The math is in ff7_world_core.h, checked against the
 * recompiled function under emulation (scripts/emu/test_world.py: memory,
 * EAX/ECX/EDX, ESP/EBP and the x87 state identical over 250,000 vertices,
 * with infinities, NaNs, w == 0 and saturating conversions).
 *
 * 0x68DAE1 colors a vertex (l = m * n, then rgb * l per channel, clamped to
 * 0..255); 0x75F68C calls it for the three vertices of every world map
 * triangle (~10% of the game thread). It is also used in fields. The math is
 * world_core_light, checked under emulation the same way (output, EAX/ECX/EDX
 * and x87 C0/C2/C3 over ~50,000 vertices); a NaN in a comparison (x87
 * unordered) goes to the original.
 *
 * 0x75F0AD builds the mesh's vertices before that: after four calls that
 * set up the matrices (left to the recompiled functions), a loop converts
 * 122 int16 vertices to float into in[], computes their projected z with a
 * second matrix and bends the world (curvature) by lowering an earlier
 * vertex: ~8% of the game thread. Native loop: world_core_mesh, checked the
 * same way against the whole recompiled function.
 *
 * 0x75F68C then draws the mesh's triangles (culling, fog, vertex colors and
 * the vertex buffer): ~29% of the game thread, the largest. Native version:
 * ff7_world_tri.h, checked by compiling it into an ARM blob that runs under
 * emulation next to the recompiled function, both calling the same
 * recompiled helpers (scripts/emu/test_world_tri.py: x86 memory, registers,
 * x87 flags and _check_event calls identical). It is not self-tested in
 * game: running both would allocate the vertex buffer twice.
 * DATA_PATH "world_tri_off.txt" turns it off alone.
 *
 * The arrays of 0x75F263 span two pages (0xE38000-0xE39FFF); if they are
 * not contiguous on the host, the original runs. DATA_PATH "world_off.txt"
 * turns both off.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_world.h"
#include "ff7_world_core.h"
#include "ff7_world_tri.h"
#include "ff7_anim.h"
#include "ff7_selftest.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH      DATA_PATH "world_off.txt"
#define SYM_PROJECT     "_Z15func_0x0075f263v"
#define SYM_CHECK_EVENT "_Z12_check_eventv"
#define SYM_MK_ADDRESS  "mk_address"
#define FPU_GOT         0xf4c82c // GOT slot: pointer to the x87 state
#define FPU_ZE          0x56     // x87 zero-divide flag (set by the recompiled fdiv)
#define FPU_TOP         0x60
#define PROJECT_PROLOGUE 0x4ff0e92d // stmdb sp!, {r4-r11, lr}
#define SELFTEST_CALLS  300
#define SYM_LIGHT       "_Z15func_0x0068dae1v"
#define SYM_MESH        "_Z15func_0x0075f0adv"
#define SYM_TRIS        "_Z15func_0x0075f68cv"
#define SYM_ALLOC       "_Z15func_0x0066e272v"
#define TRIS_PROLOGUE   0x4ff0e92d // stmdb sp!, {r4-r11, lr}
#define TRI_OFF_SWITCH  DATA_PATH "world_tri_off.txt"
#define MESH_PROLOGUE   PROJECT_PROLOGUE
#define VA_M2           0xe39778u // second matrix (0x75F0AD's setup)
#define VA_CAMERA       0xe360f4u
#define VA_K1           0x7b7ef0u // float 0.015625
#define VA_K2           0x7b7ef4u // float 0.03125
static const char * const mesh_setup_syms[4] = {
    "_Z15func_0x0066100dv", "_Z15func_0x006617e9v", "_Z15func_0x0069c69fv", "_Z15func_0x0067d2bfv",
};
#define LIGHT_PROLOGUE  0xb083b5f0 // push {r4-r7, lr}; sub sp, #12
#define VA_ZERO         0x7b7b3cu  // float 0.0 (.rdata)
#define VA_SAT          0x7b7b44u  // float 255.0
#define FPU_C0          0x5c
#define FPU_C2          0x5e
#define FPU_C3          0x64

// ff7.exe addresses
#define VA_PAGES        0xe38000u // two pages: in, proj, out, matrix, flags
#define VA_IN           0xe38338u
#define VA_PROJ         0xe38938u
#define VA_OUT          0xe38f38u
#define VA_MATRIX       0xe39738u
#define VA_FLAGS        0xe397b8u
#define VA_END          (VA_FLAGS + 4 * WORLD_PROJECTED)
#define VA_K            0x7b7ef8u // float 1.0 (.rdata)
#define VA_LEFT         0xe2c424u // int: clip area
#define VA_TOP          0xe2c428u
#define VA_WIDTH        0xde68f8u
#define VA_HEIGHT       0xde67e8u

// Calls to _check_event() the original makes: its own, 126 x 0x66CE40 and
// 246 x 0x75F090
#define CHECK_EVENTS    (1 + WORLD_VERTICES + 2 * WORLD_PROJECTED)

enum { EAX = 0, ECX = 1, EDX = 2, ESP = 4, EBP = 5 };

typedef uint8_t * (* mk_address_fn)(uint32_t addr);
typedef void (* check_event_fn)(void);

static mk_address_fn mk_address;
static check_event_fn check_event;
static volatile uint32_t * regs;
static uint8_t * fpu;
static so_hook project_hook;

static int selftest_left, disabled;
static unsigned long long calls, fallbacks;

static so_hook light_hook;
static int light_selftest_left, light_disabled;
static unsigned long long light_calls, light_fallbacks;

static so_hook tris_hook;
static world_env tri_env;
static unsigned long long tris_calls, tris_fallbacks;

static so_hook mesh_hook;
static void (* mesh_setup[4])(void);
static int mesh_selftest_left, mesh_disabled;
static unsigned long long mesh_calls, mesh_fallbacks;

static inline uint32_t rd32(uint32_t va) {
    uint32_t v;
    memcpy(&v, mk_address(va), 4);
    return v;
}

// Host address of VA_PAGES if both pages are mapped back to back, else NULL
static uint8_t * pages(void) {
    uint8_t * lo = mk_address(VA_PAGES), * hi = mk_address(VA_PAGES + 0x1000);
    return hi == lo + 0x1000 ? lo : NULL;
}

// Everything 0x75F263 does to x86 memory, on a copy of the two pages if
// `mem` is not the real one. Returns 1 if a w was 0.
static int project(uint8_t * mem) {
#define AT(type, va) ((type) (mem + ((va) - VA_PAGES)))
    int32_t left = (int32_t) rd32(VA_LEFT), top = (int32_t) rd32(VA_TOP);
    int32_t right = (int32_t) (rd32(VA_WIDTH) + (uint32_t) left);
    int32_t bottom = (int32_t) (rd32(VA_HEIGHT) + (uint32_t) top);
    float k;
    memcpy(&k, mk_address(VA_K), 4);
    return world_core_project(AT(const float *, VA_MATRIX), AT(const float (*)[3], VA_IN),
                              AT(float (*)[4], VA_OUT), AT(float (*)[3], VA_PROJ),
                              AT(int32_t *, VA_FLAGS), k, left, top, right, bottom);
#undef AT
}

static uint8_t snapshot[VA_END - VA_PAGES];

static void project_native(void) {
    uint8_t * mem = disabled ? NULL : pages();
    if (!mem) {
        fallbacks++;
        SO_CONTINUE(int, project_hook);
        return;
    }

    if (selftest_left > 0) {
        selftest_left--;
        memcpy(snapshot, mem, sizeof(snapshot));
        int zd = project(snapshot);
        uint8_t ze = fpu[FPU_ZE], top = fpu[FPU_TOP];
        uint32_t esp = regs[ESP];
        SO_CONTINUE(int, project_hook);
        if (memcmp(snapshot, mem, sizeof(snapshot)) || regs[EAX] != 123 || regs[ECX] != 0 || regs[EDX] != 126
            || regs[ESP] != esp + 4 || fpu[FPU_TOP] != top || fpu[FPU_ZE] != (zd ? 1 : ze)) {
            uint32_t off = 0;
            while (off < sizeof(snapshot) && snapshot[off] == mem[off]) off++;
            l_error("ff7_world selftest MISMATCH at VA 0x%x (eax %x ecx %x edx %x, ze %d native %d)"
                    " - native projection disabled", VA_PAGES + off, regs[EAX], regs[ECX], regs[EDX],
                    fpu[FPU_ZE], zd);
            disabled = 1;
        } else if (selftest_left == 0) {
            l_info("ff7_world selftest ok: %d calls identical", SELFTEST_CALLS);
        }
        return;
    }

    if (project(mem)) fpu[FPU_ZE] = 1;
    regs[EAX] = 123; // the x86 loop's last index, count and next index
    regs[ECX] = 0;
    regs[EDX] = 126;
    for (int i = 0; i < CHECK_EVENTS; i++) check_event();
    regs[ESP] += 4; // ret (EBP is restored by the original's epilogue: untouched here)
    calls++;
}

// Reads n bytes from x86 memory (handles page crossings)
static inline void xread(uint32_t va, void * dst, uint32_t n_bytes) {
    uint8_t * d = dst;
    while (n_bytes) {
        uint32_t in_page = 0x1000 - (va & 0xfff);
        uint32_t n = n_bytes < in_page ? n_bytes : in_page;
        memcpy(d, mk_address(va), n);
        d += n;
        va += n;
        n_bytes -= n;
    }
}

static inline uint32_t stack_arg(uint32_t esp, int i) {
    uint32_t v;
    xread(esp + 4 * i, &v, 4);
    return v;
}

typedef struct {
    uint8_t out[4];
    uint32_t edx;
    int lt, eq;
} light_result;

// 0x68DAE1's results; 0 when a NaN takes part in a comparison
static int light(uint32_t m_va, uint32_t n_va, uint32_t rgb_va, uint32_t src_va, uint32_t out_va, light_result * r) {
    float m[12], n[3], rgb[3], zero, sat;
    xread(m_va, m, sizeof(m));
    xread(n_va, n, sizeof(n));
    xread(rgb_va, rgb, sizeof(rgb));
    memcpy(&zero, mk_address(VA_ZERO), 4);
    memcpy(&sat, mk_address(VA_SAT), 4);
    int path[3];
    if (!world_core_light(m, n, rgb, zero, sat, r->out, path, &r->lt, &r->eq)) return 0;
    r->out[3] = *mk_address(src_va + 3);
    uint32_t sel = world_core_light_edx(path);
    uint32_t hi = sel == WORLD_EDX_RGB ? rgb_va : sel == WORLD_EDX_OUT ? out_va : m_va;
    r->edx = (hi & ~0xffu) | r->out[3];
    return 1;
}

static void light_native(void) {
    uint32_t esp = regs[ESP];
    uint32_t m = stack_arg(esp, 1), n = stack_arg(esp, 2), rgb = stack_arg(esp, 3);
    uint32_t src = stack_arg(esp, 4), out = stack_arg(esp, 5);

    light_result r;
    if (light_disabled || !light(m, n, rgb, src, out, &r)) { // NaN: the original
        light_fallbacks++;
        SO_CONTINUE(int, light_hook);
        return;
    }

    if (light_selftest_left > 0) {
        light_selftest_left--;
        SO_CONTINUE(int, light_hook);
        uint8_t orig[4];
        xread(out, orig, 4);
        if (memcmp(orig, r.out, 4) || regs[EAX] != out || regs[ECX] != src || regs[EDX] != r.edx
            || fpu[FPU_C0] != r.lt || fpu[FPU_C2] != 0 || fpu[FPU_C3] != r.eq) {
            l_error("ff7_world 0x68DAE1 selftest MISMATCH: native %02x%02x%02x%02x edx %08x c0 %d c3 %d, original "
                    "%02x%02x%02x%02x edx %08x c0 %d c3 %d - disabled", r.out[0], r.out[1], r.out[2], r.out[3],
                    r.edx, r.lt, r.eq, orig[0], orig[1], orig[2], orig[3], regs[EDX], fpu[FPU_C0], fpu[FPU_C3]);
            light_disabled = 1;
        } else if (light_selftest_left == 0) {
            l_info("ff7_world 0x68DAE1 selftest ok: %d vertices identical", SELFTEST_CALLS);
        }
        return;
    }

    // Same order as the x86 code: out[2], out[1], out[0], out[3]
    static const int order[4] = { 2, 1, 0, 3 };
    for (int i = 0; i < 4; i++) *mk_address(out + order[i]) = r.out[order[i]];
    regs[EAX] = out;
    regs[ECX] = src;
    regs[EDX] = r.edx;
    fpu[FPU_C0] = (uint8_t) r.lt;
    fpu[FPU_C2] = 0;
    fpu[FPU_C3] = (uint8_t) r.eq;
    check_event(); // 0x66CFE8's
    check_event();
    regs[ESP] = esp + 4; // ret
    light_calls++;
}

static inline void xwr32(uint32_t va, uint32_t v) {
    if ((va & 0xfff) <= 0xffc) memcpy(mk_address(va), &v, 4);
    else for (int i = 0; i < 4; i++) *mk_address(va + i) = (uint8_t) (v >> (8 * i));
}

/*
 * 0x75F0AD (src, step). The four setup calls run as the x86 code makes
 * them: EBP = the frame, arguments pushed below EBP - 0x34, ESP lowered by 4
 * for the return address (the recompiled call does not store it).
 */
static void mesh_native(void) {
    uint32_t esp = regs[ESP], ebp_in = regs[EBP];
    uint8_t * mem = mesh_disabled ? NULL : pages();
    if (!mem) {
        mesh_fallbacks++;
        SO_CONTINUE(int, mesh_hook);
        return;
    }
    uint32_t src_va = stack_arg(esp, 1), step = stack_arg(esp, 2);
    uint32_t ebp = esp - 4;

    uint8_t * before = NULL;
    if (mesh_selftest_left > 0) {
        // The original, then the native version from the same state
        mesh_selftest_left--;
        before = snapshot;
        memcpy(before, mem, sizeof(snapshot));
    }

    xwr32(ebp, ebp_in); // push ebp
    regs[EBP] = ebp;
    xwr32(ebp - 0x38, VA_M2);
    regs[ESP] = ebp - 0x3c;
    mesh_setup[0](); // 0x66100d (0xe39778)
    xwr32(ebp - 0x3c, regs[EAX]);
    regs[ESP] = ebp - 0x40;
    mesh_setup[1](); // 0x6617e9 (eax, 0xe39778)
    regs[EAX] = rd32(VA_CAMERA);
    xwr32(ebp - 0x38, regs[EAX]);
    xwr32(ebp - 0x3c, VA_M2);
    regs[ESP] = ebp - 0x40;
    mesh_setup[2](); // 0x69c69f (0xe39778, [0xe360f4])
    xwr32(ebp - 0x38, VA_MATRIX);
    xwr32(ebp - 0x3c, VA_M2);
    regs[ESP] = ebp - 0x40;
    mesh_setup[3](); // 0x67d2bf (0xe39778, 0xe39738)

    static int16_t src[4 * WORLD_MESH_VERTICES];
    xread(src_va, src, sizeof(src));
    int32_t c = (int32_t) ((rd32(VA_WIDTH) >> 1) + rd32(VA_LEFT));
    int32_t v0 = (int32_t) rd32(ebp - 4);
    float k1, k2;
    memcpy(&k1, mk_address(VA_K1), 4);
    memcpy(&k2, mk_address(VA_K2), 4);
#define AT(type, va) ((type) (mem + ((va) - VA_PAGES)))
    int32_t eax, ecx;
    int zd = world_core_mesh(src, (int32_t) step, AT(const float *, VA_MATRIX), AT(const float *, VA_M2),
                             AT(float (*)[3], VA_IN), AT(float (*)[3], VA_PROJ), c, v0, k1, k2, &eax, &ecx);
#undef AT
    if (zd) fpu[FPU_ZE] = 1;
    regs[EAX] = (uint32_t) eax;
    regs[ECX] = (uint32_t) ecx;
    regs[EDX] = (WORLD_MESH_VERTICES - 1) * 12;
    regs[EBP] = ebp_in; // pop ebp
    for (int i = 0; i < 1 + 4 * WORLD_MESH_VERTICES; i++) check_event();
    regs[ESP] = esp + 4; // ret
    mesh_calls++;

    if (before) {
        // Native result in `mem`; redo with the original from `before`
        static uint8_t native[sizeof(snapshot)];
        uint32_t n_eax = regs[EAX], n_ecx = regs[ECX];
        memcpy(native, mem, sizeof(native));
        memcpy(mem, before, sizeof(snapshot));
        regs[ESP] = esp;
        regs[EBP] = ebp_in;
        SO_CONTINUE(int, mesh_hook);
        if (memcmp(native, mem, sizeof(native)) || regs[EAX] != n_eax || regs[ECX] != n_ecx
            || regs[EDX] != (WORLD_MESH_VERTICES - 1) * 12 || regs[ESP] != esp + 4 || regs[EBP] != ebp_in) {
            uint32_t off = 0;
            while (off < sizeof(native) && native[off] == mem[off]) off++;
            l_error("ff7_world 0x75F0AD selftest MISMATCH at VA 0x%x (eax %x/%x ecx %x/%x) - disabled",
                    VA_PAGES + off, regs[EAX], n_eax, regs[ECX], n_ecx);
            mesh_disabled = 1;
        } else if (mesh_selftest_left == 0) {
            l_info("ff7_world 0x75F0AD selftest ok: %d meshes identical", SELFTEST_CALLS);
        }
    }
}

// 0x75F68C (tri, tri_end, normals, arg4)
static void tris_native(void) {
    tri_env.pages = pages();
    if (!world_tri_run(&tri_env)) {
        tris_fallbacks++;
        SO_CONTINUE(int, tris_hook);
        return;
    }
    tris_calls++;
}

static void tris_patch(uintptr_t light_fn) {
    if (file_exists(TRI_OFF_SWITCH)) {
        l_info("ff7_world: native 0x75F68C disabled by %s", TRI_OFF_SWITCH);
        return;
    }
    uintptr_t fn = so_symbol(&so_mod, SYM_TRIS), alloc = so_symbol(&so_mod, SYM_ALLOC);
    if (!fn || !alloc || *(uint32_t *) (fn & ~1) != TRIS_PROLOGUE) {
        l_warn("ff7_world: unexpected .so layout, native 0x75F68C disabled");
        return;
    }
    tri_env.mk_address = mk_address;
    tri_env.check_event = check_event;
    tri_env.regs = regs;
    tri_env.fpu = fpu;
    tri_env.f_68dae1 = (void (*)(void)) light_fn; // its entry: the native hook
    tri_env.f_66e272 = (void (*)(void)) alloc;
    tris_hook = hook_addr(fn, (uintptr_t) &tris_native);
    l_info("ff7_world: native world map triangles (0x75F68C) installed");
}

void ff7_world_patch(void) {
    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_world: disabled by %s", OFF_SWITCH);
        return;
    }
    uintptr_t fn = so_symbol(&so_mod, SYM_PROJECT);
    check_event = (check_event_fn) so_symbol(&so_mod, SYM_CHECK_EVENT);
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);
    regs = ff7_x86_regs();
    fpu = *(uint8_t **) (so_mod.text_base + FPU_GOT);
    uintptr_t f = (uintptr_t) fpu;
    if (!fn || !check_event || !mk_address || !regs || *(uint32_t *) (fn & ~1) != PROJECT_PROLOGUE
        || f < so_mod.text_base || f >= so_mod.text_base + 0x1498e40) {
        l_warn("ff7_world: unexpected .so layout, native world map projection disabled");
        return;
    }
    selftest_left = ff7_selftest_count(SELFTEST_CALLS);
    project_hook = hook_addr(fn, (uintptr_t) &project_native);
    l_info("ff7_world: native world map projection (0x75F263) installed (selftest on %d calls)", selftest_left);

    uintptr_t lfn = so_symbol(&so_mod, SYM_LIGHT);
    if (!lfn || *(uint32_t *) (lfn & ~1) != LIGHT_PROLOGUE) {
        l_warn("ff7_world: unexpected .so layout, native 0x68DAE1 disabled");
        return;
    }
    light_selftest_left = ff7_selftest_count(SELFTEST_CALLS);
    light_hook = hook_addr(lfn, (uintptr_t) &light_native);
    l_info("ff7_world: native vertex color (0x68DAE1) installed (selftest on %d vertices)", light_selftest_left);
    tris_patch(lfn);

    uintptr_t mfn = so_symbol(&so_mod, SYM_MESH);
    for (int i = 0; i < 4; i++) mesh_setup[i] = (void (*)(void)) so_symbol(&so_mod, mesh_setup_syms[i]);
    if (!mfn || *(uint32_t *) (mfn & ~1) != MESH_PROLOGUE || !mesh_setup[0] || !mesh_setup[1] || !mesh_setup[2]
        || !mesh_setup[3]) {
        l_warn("ff7_world: unexpected .so layout, native 0x75F0AD disabled");
        return;
    }
    mesh_selftest_left = ff7_selftest_count(SELFTEST_CALLS);
    mesh_hook = hook_addr(mfn, (uintptr_t) &mesh_native);
    l_info("ff7_world: native world map mesh vertices (0x75F0AD) installed (selftest on %d meshes)",
           mesh_selftest_left);
}

void ff7_world_report(double seconds) {
    if (seconds <= 0 || (!calls && !fallbacks && !light_calls && !light_fallbacks && !mesh_calls && !mesh_fallbacks))
        return;
    l_info("world map meshes/s: built %.0f native, %.0f original; projected %.0f native, %.0f original | colored"
           " vertices/s (0x68DAE1): %.0f native, %.0f original", mesh_calls / seconds, mesh_fallbacks / seconds,
           calls / seconds, fallbacks / seconds, light_calls / seconds, light_fallbacks / seconds);
    calls = fallbacks = light_calls = light_fallbacks = mesh_calls = mesh_fallbacks = 0;
    if (tris_calls || tris_fallbacks) {
        l_info("world map triangles/s: %.0f seen, %.0f drawn (meshes: %.0f native, %.0f original; %.0f buffer"
               " allocations by the original, %.0f NaN compares)", tri_env.counters[0] / seconds,
               tri_env.counters[1] / seconds, tris_calls / seconds, tris_fallbacks / seconds,
               tri_env.counters[2] / seconds, tri_env.counters[3] / seconds);
        tris_calls = tris_fallbacks = 0;
        for (int i = 0; i < 4; i++) tri_env.counters[i] = 0;
    }
}
