/*
 * Native triangle renderer for libjni_ff7.so.
 *
 * C_006A8002 (count, indices, vertices, matrix) is the porters' C++ path for
 * transformed triangle lists: per vertex it calls the 4x4 transform, then
 * fake_glColor4ub and glVertex3f of their GL 1.x emulation, each reloading
 * its globals through the GOT. Profiling showed it with those helpers at
 * ~13% of the game thread in busy Sector 7 scenes. The replacement does the
 * same work inline (see ff7_tri_core.h for the exact behavior), between the
 * original glBegin() and glEnd().
 *
 * Checked offline against the original under emulation (scripts/emu/
 * test_tri.py: VERTICES, pCurVertex and the current color identical) and on
 * the first calls in game: the native result is built in a scratch buffer,
 * then the original runs and VERTICES is compared (glEnd draws it but leaves
 * the contents in place).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Bit-exact float math (the build may use -ffast-math)
#pragma GCC optimize ("no-fast-math", "fp-contract=off")

#include "ff7_tri.h"
#include "ff7_tri_core.h"
#include "ff7_tri2_core.h"
#include "ff7_selftest.h"

#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define SYM_TRI     "_Z10C_006A8002iPtP19t_dx_rend_vertex_20P8t_matrix"
/* Highway minigame renderer: same layout but no matrix multiply */
#define SYM_TRI2    "_Z10C_006A8A3EiPtP19t_dx_rend_vertex_20"
#define SYM_BEGIN   "glBegin"
#define SYM_END     "glEnd"
#define TRI_PROLOGUE  0x47f0e92d /* push.w {r4-r10, lr} — C_006A8002 */
#define TRI2_PROLOGUE 0x43f8e92d /* push.w {r3-r9, lr}  — C_006A8A3E */
#define CUR_VERTEX  0xf4f4d8    // pCurVertex
#define LAST_VERTEX 0xf4f4dc    // pLastVertex
#define VERTICES    0x146263c
#define CUR_R       0x1462587
#define CUR_G       0x1462586
#define CUR_B       0x1462585
#define CUR_A       0x1462584
#define CUR_U       0x146258c
#define CUR_V       0x1462588
#define KEEP_ALPHA  0x141fad8   // read by C_006A8002 (literal, not through the GOT)
#define SELFTEST_CALLS 300

typedef void (* begin_fn)(int mode);
typedef void (* end_fn)(void);

static so_hook tri_hook;
static so_hook tri2_hook;        /* C_006A8A3E (highway) */
static begin_fn gl_begin;
static end_fn gl_end;
static tri_state state;
static uint8_t * vertices;
static int selftest_left = SELFTEST_CALLS, disabled;
static unsigned long long calls, verts_out;

/* --- C_006A8A3E stats --- */
static int tri2_disabled;
static unsigned long long tri2_calls, tri2_verts_out;

static int selftest(int count, const uint16_t * idx, const uint8_t * verts, const float * m) {
    size_t room = *state.last > vertices ? (size_t) (*state.last - vertices) : 0;
    uint8_t * scratch = malloc(room + 0x18);
    if (!scratch) return 1;
    uint8_t * cur = scratch, * last = scratch + room;
    uint8_t col[4] = { *state.r, *state.g, *state.b, *state.a };
    tri_state s = state;
    s.cur = &cur;
    s.last = &last;
    s.r = &col[0];
    s.g = &col[1];
    s.b = &col[2];
    s.a = &col[3];
    tri_core_draw(&s, count, idx, verts, m);
    size_t n = (size_t) (cur - scratch);

    // VERTICES right after the native output must stay as it is
    uint8_t after[0x18];
    int check_after = n + 0x18 <= room;
    if (check_after) memcpy(after, vertices + n, 0x18);

    SO_CONTINUE(int, tri_hook, count, idx, verts, m);

    int ok = !memcmp(vertices, scratch, n) && (!check_after || !memcmp(vertices + n, after, 0x18))
             && col[0] == *state.r && col[1] == *state.g && col[2] == *state.b && col[3] == *state.a;
    if (!ok) {
        size_t at = 0;
        while (at < n && vertices[at] == scratch[at]) at++;
        l_error("ff7_tri selftest MISMATCH (count %d, %u vertices, first difference at byte %u) - native "
                "triangle renderer disabled", count, (unsigned) (n / 0x18), (unsigned) at);
    }
    free(scratch);
    return ok;
}

static void tri_native(int count, const uint16_t * idx, const uint8_t * verts, const float * m) {
    if (disabled) {
        SO_CONTINUE(int, tri_hook, count, idx, verts, m);
        return;
    }
    if (selftest_left > 0) {
        selftest_left--;
        if (!selftest(count, idx, verts, m)) disabled = 1;
        else if (selftest_left == 0) l_info("ff7_tri selftest ok: %d calls identical", SELFTEST_CALLS);
        return;
    }
    gl_begin(4); // GL_TRIANGLES
    uint8_t * start = *state.cur;
    tri_core_draw(&state, count, idx, verts, m);
    verts_out += (*state.cur - start) / 0x18;
    gl_end();
    calls++;
}

/*
 * C_006A8A3E — highway minigame triangle renderer.
 * Already inside a glBegin/glEnd block; tail-calls glEnd().
 * No matrix multiply, no keep_alpha: straight copy from vertex buffer.
 *
 * Checked offline via scripts/emu/test_tri2.py (x/y/z and u/v match;
 * the emulator cannot verify colours because fake_glColor4ub uses GOT
 * indirection that ff7emu.py does not resolve). The colour logic mirrors
 * ff7_tri_core.h exactly: input offsets from the disassembly of the
 * original function are byte-for-byte the same as what the original writes
 * to the global CUR_R/G/B/A and then glVertex3f copies to the buffer.
 * If the Vita selftest (selftest_on.txt) flags a mismatch, tri2_disabled
 * is set and all calls fall through to the original.
 */
static void tri2_native(int count, const uint16_t * idx, const uint8_t * verts) {
    if (tri2_disabled) {
        SO_CONTINUE(int, tri2_hook, count, idx, verts);
        return;
    }
    gl_begin(4); /* GL_TRIANGLES */
    uint8_t * start = *state.cur;
    tri2_core_draw(&state, count, idx, verts);
    tri2_verts_out += (*state.cur - start) / 0x18;
    gl_end();
    tri2_calls++;
}

void ff7_tri_patch(void) {
    uintptr_t fn = so_symbol(&so_mod, SYM_TRI);
    gl_begin = (begin_fn) so_symbol(&so_mod, SYM_BEGIN);
    gl_end = (end_fn) so_symbol(&so_mod, SYM_END);
    if (!fn || !gl_begin || !gl_end || *(uint32_t *) (fn & ~1) != TRI_PROLOGUE) {
        l_warn("ff7_tri: unexpected .so layout, native triangle renderer disabled");
        return;
    }
    uintptr_t b = so_mod.text_base;
    state.cur = (uint8_t **) (b + CUR_VERTEX);
    state.last = (uint8_t * const *) (b + LAST_VERTEX);
    state.r = (uint8_t *) (b + CUR_R);
    state.g = (uint8_t *) (b + CUR_G);
    state.b = (uint8_t *) (b + CUR_B);
    state.a = (uint8_t *) (b + CUR_A);
    state.u = (const uint32_t *) (b + CUR_U);
    state.v = (const uint32_t *) (b + CUR_V);
    state.keep_alpha = (const uint32_t *) (b + KEEP_ALPHA);
    vertices = (uint8_t *) (b + VERTICES);
    selftest_left = ff7_selftest_count(SELFTEST_CALLS);
    tri_hook = hook_addr(fn, (uintptr_t) &tri_native);
    l_info("ff7_tri: native triangle renderer installed (selftest on %d calls)", selftest_left);

    /* C_006A8A3E — highway renderer (no matrix, same vertex buffer) */
    uintptr_t fn2 = so_symbol(&so_mod, SYM_TRI2);
    if (!fn2 || *(uint32_t *) (fn2 & ~1) != TRI2_PROLOGUE) {
        l_warn("ff7_tri: C_006A8A3E not found or unexpected prologue, highway renderer not hooked");
        return;
    }
    tri2_hook = hook_addr(fn2, (uintptr_t) &tri2_native);
    l_info("ff7_tri: native highway triangle renderer (C_006A8A3E) installed");
}

void ff7_tri_report(double seconds) {
    if (seconds <= 0) return;
    if (calls)
        l_info("triangle lists/s: %.0f native, %.0f vertices/s", calls / seconds, verts_out / seconds);
    if (tri2_calls)
        l_info("highway triangles/s (C_006A8A3E): %.0f native, %.0f vertices/s", tri2_calls / seconds, tri2_verts_out / seconds);
    calls = verts_out = 0;
    tri2_calls = tri2_verts_out = 0;
}
