/*
 * Native renderer for C_006A8A3E (highway minigame triangle renderer),
 * shared by ff7_tri.c and the offline test.
 *
 * C_006A8A3E(count, indices, verts) — called from within a glBegin/glEnd
 * block by the highway module.  The vertices are already in screen space
 * (no matrix multiply): each t_dx_rend_vertex_20 is 32 bytes:
 *   +0   float x
 *   +4   float y
 *   +8   float z
 *   +12  (unused here)
 *   +16  uint8  b
 *   +17  uint8  g
 *   +18  uint8  r
 *   +19  uint8  a
 *   +20  (pad)
 *   +24  float u
 *   +28  float v
 *
 * Per triangle: indices are uint16, stride is sizeof(vertex)=32 bytes.
 * For each of the three vertices:
 *   fake_glColor4ub(r, g, b, a)          (offsets 18, 17, 16, 19)
 *   fake_glTexCoord2f(u, v)               (offsets 24, 28  — loaded as floats)
 *   fake_glVertex3f(x, y, z)             (offsets 0, 4, 8 — loaded as floats)
 *
 * No w-divide, no matrix, no keep_alpha logic.  The function ends with a
 * tail-call to glEnd(), so the native version must do the same.
 *
 * Bit-exact: build without -ffast-math and without FP contraction.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_TRI2_CORE_H
#define FF7_TRI2_CORE_H

#include <stdint.h>
#include <string.h>

/* Emit one vertex from C_006A8A3E's simpler vertex layout.
 * Colour bytes in the .so come out in B G R A order at +16..+19.
 * fake_glColor4ub is called as (r=+18, g=+17, b=+16, a=+19). */
static inline void tri2_core_emit(const uint8_t * vtx, uint8_t ** cur,
                                   const uint8_t * last, uint8_t col[4]) {
    /* colour: r=+18 g=+17 b=+16 a=+19 */
    col[0] = vtx[18];  /* r */
    col[1] = vtx[17];  /* g */
    col[2] = vtx[16];  /* b */
    col[3] = vtx[19];  /* a */

    float x, y, z, u, v;
    memcpy(&x, vtx +  0, 4);
    memcpy(&y, vtx +  4, 4);
    memcpy(&z, vtx +  8, 4);
    memcpy(&u, vtx + 24, 4);
    memcpy(&v, vtx + 28, 4);

    uint8_t * p = *cur;
    if (p >= last) return;
    memcpy(p,      &x, 4);
    memcpy(p +  4, &y, 4);
    memcpy(p +  8, &z, 4);
    memcpy(p + 12, col, 4);
    memcpy(p + 16, &u, 4);
    memcpy(p + 20, &v, 4);
    *cur = p + 0x18;
}

/*
 * The loop between glBegin(GL_TRIANGLES) and glEnd() for C_006A8A3E.
 * `s` is the same tri_state as used by tri_core_draw (shares pCurVertex,
 * pLastVertex and the current-colour pointers).
 */
static inline void tri2_core_draw(const tri_state * s, int count,
                                   const uint16_t * idx,
                                   const uint8_t * verts) {
    uint8_t * cur = *s->cur;
    const uint8_t * last = *s->last;
    uint8_t col[4];
    int emitted = 0;

    for (; count > 0; count -= 3, idx += 3) {
        const uint8_t * v0 = verts + 32u * idx[0];
        const uint8_t * v1 = verts + 32u * idx[1];
        const uint8_t * v2 = verts + 32u * idx[2];
        tri2_core_emit(v0, &cur, last, col);
        tri2_core_emit(v1, &cur, last, col);
        tri2_core_emit(v2, &cur, last, col);
        emitted = 1;
    }

    *s->cur = cur;
    if (emitted) {
        *s->r = col[0];
        *s->g = col[1];
        *s->b = col[2];
        *s->a = col[3];
    }
}

#endif /* FF7_TRI2_CORE_H */
