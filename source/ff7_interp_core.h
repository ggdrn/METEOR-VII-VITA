/*
 * Battle at 30 fps, phase 0 (docs/BATALHA-30FPS.md): records the draws of a
 * battle frame (state key, primitive, x/y and u/v of each vertex, all in
 * screen space) and matches them with the previous frame's, to measure how
 * much of the image an interpolated in-between frame could move. Shared by
 * ff7_interp.c and the host test scripts/test_interp.c. Plain C, no Vita
 * headers.
 *
 * Matching, per pair of frames A (previous) and B (current):
 *  - draws: by state key, in order (the k-th draw with key K in B pairs with
 *    the k-th with key K in A);
 *  - vertices, fast case: same primitive, same count and identical u/v
 *    (static meshes): vertex i with vertex i;
 *  - triangles, otherwise (GL_TRIANGLES): by a hash of the u/v of their 3
 *    vertices, the first unused equal triangle of A in insertion order (so
 *    repeated u/v pair up in order);
 *  - the rest has no pair (it would be drawn as in B).
 * A pair of frames where most moving vertices jump more than IP_CUT_PX is
 * counted as a cut (camera cut, scene change) and left out of the
 * displacement figures.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_INTERP_CORE_H
#define FF7_INTERP_CORE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define IP_TRIANGLES 4      // GL_TRIANGLES
#define IP_MOVE_PX   0.01f  // below: a vertex that stays
#define IP_CUT_PX    96.0f  // above: a jump, not a motion

typedef struct {
    uint32_t key, mode, first, count; // first: index in the frame's vertices
} ip_draw;

typedef struct {
    float x, y;
    uint32_t u, v; // bit patterns: compared, never computed with
} ip_vert;

typedef struct {
    ip_draw * d;
    ip_vert * v;
    uint32_t nd, nv, maxd, maxv;
    uint32_t overflow; // draws or vertices beyond the buffers: not matched
} ip_frame;

typedef struct {
    uint32_t key;
    int32_t head;
} ip_keyslot;

typedef struct {
    uint32_t h;
    uint32_t tri; // triangle + 1 (0: empty), top bit: used
} ip_trislot;

// Scratch for ip_match: sized by the caller
typedef struct {
    ip_keyslot * keys;   // key_size (power of two) >= 2 * maxd
    int32_t * next;      // maxd
    ip_trislot * tris;   // tri_size (power of two)
    uint32_t key_size, tri_size;
} ip_scratch;

typedef struct {
    uint32_t pairs, cuts, overflows;
    uint64_t draws, draws_matched, verts, fast, by_uv, unmatched, moving, far;
    uint64_t draws_tri, draws_other;
    double dsum;
    float dmax;
} ip_stats;

static inline void ip_reset(ip_frame * f) {
    f->nd = f->nv = 0;
    f->overflow = 0;
}

static inline uint32_t ip_mix(uint32_t h, uint32_t w) {
    h ^= w;
    h *= 0x85ebca6bu;
    return h ^ (h >> 13);
}

// One draw: `count` vertices, x/y floats at pos (stride ps), u/v floats at uv (stride us; NULL: zeros)
static inline int ip_add(ip_frame * f, uint32_t key, uint32_t mode, uint32_t count, const uint8_t * pos, uint32_t ps,
                         const uint8_t * uv, uint32_t us) {
    if (f->nd >= f->maxd || count > f->maxv - f->nv) {
        f->overflow = 1;
        return 0;
    }
    ip_draw * d = &f->d[f->nd++];
    d->key = key;
    d->mode = mode;
    d->first = f->nv;
    d->count = count;
    ip_vert * o = f->v + f->nv;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(&o[i].x, pos + i * ps, 8);
        if (uv) memcpy(&o[i].u, uv + i * us, 8);
        else o[i].u = o[i].v = 0;
    }
    f->nv += count;
    return 1;
}

typedef struct {
    uint64_t moving, far;
    double dsum;
    float dmax;
} ip_disp;

static inline void ip_moved(const ip_vert * a, const ip_vert * b, ip_disp * p) {
    float dx = b->x - a->x, dy = b->y - a->y;
    float d = sqrtf(dx * dx + dy * dy);
    if (!(d > IP_MOVE_PX)) return; // also NaN
    p->moving++;
    p->dsum += d;
    if (d > p->dmax) p->dmax = d;
    if (d > IP_CUT_PX) p->far++;
}

static inline int ip_same_uv(const ip_vert * a, const ip_vert * b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i].u != b[i].u || a[i].v != b[i].v) return 0;
    return 1;
}

static inline uint32_t ip_tri_hash(const ip_vert * t) {
    uint32_t h = 0x9e3779b9u;
    for (int k = 0; k < 3; k++) h = ip_mix(ip_mix(h, t[k].u), t[k].v);
    return h ? h : 1;
}

// Triangles of b paired with triangles of a by u/v; returns the vertices matched (or -1: table too small)
static inline int64_t ip_match_tris(const ip_vert * a, uint32_t na, const ip_vert * b, uint32_t nb, ip_scratch * s,
                                    ip_disp * p) {
    uint32_t ta = na / 3, tb = nb / 3, size = 16;
    while (size < 2 * ta) size <<= 1;
    if (size > s->tri_size) return -1;
    uint32_t mask = size - 1;
    memset(s->tris, 0, size * sizeof(ip_trislot));
    for (uint32_t t = 0; t < ta; t++) {
        uint32_t h = ip_tri_hash(a + 3 * t), i = h & mask;
        while (s->tris[i].tri) i = (i + 1) & mask;
        s->tris[i].h = h;
        s->tris[i].tri = t + 1;
    }
    int64_t matched = 0;
    for (uint32_t t = 0; t < tb; t++) {
        const ip_vert * bt = b + 3 * t;
        uint32_t h = ip_tri_hash(bt), i = h & mask;
        for (; s->tris[i].tri; i = (i + 1) & mask) {
            ip_trislot * e = &s->tris[i];
            if (e->h != h || (e->tri & 0x80000000u)) continue;
            const ip_vert * at = a + 3 * (e->tri - 1);
            if (!ip_same_uv(at, bt, 3)) continue;
            e->tri |= 0x80000000u;
            for (int k = 0; k < 3; k++) ip_moved(at + k, bt + k, p);
            matched += 3;
            break;
        }
    }
    return matched;
}

// Matches B with A and adds the figures to st
static inline void ip_match(const ip_frame * A, const ip_frame * B, ip_scratch * s, ip_stats * st) {
    if (A->overflow || B->overflow) {
        st->overflows++;
        return;
    }
    // Keys of A: per key, its draws in order (head, then next[])
    uint32_t kmask = s->key_size - 1;
    for (uint32_t i = 0; i <= kmask; i++) s->keys[i].head = -2; // empty
    for (int32_t i = (int32_t) A->nd - 1; i >= 0; i--) {
        uint32_t k = A->d[i].key, j = ip_mix(k, 0x27d4eb2fu) & kmask;
        while (s->keys[j].head != -2 && s->keys[j].key != k) j = (j + 1) & kmask;
        s->next[i] = s->keys[j].head == -2 ? -1 : s->keys[j].head;
        s->keys[j].key = k;
        s->keys[j].head = i;
    }
    ip_disp p = { 0, 0, 0.0, 0.0f };
    uint64_t fast = 0, by_uv = 0, unmatched = 0, matched_draws = 0;
    for (uint32_t j = 0; j < B->nd; j++) {
        const ip_draw * bd = &B->d[j];
        if (bd->mode == IP_TRIANGLES) st->draws_tri++;
        else st->draws_other++;
        uint32_t k = bd->key, q = ip_mix(k, 0x27d4eb2fu) & kmask;
        while (s->keys[q].head != -2 && s->keys[q].key != k) q = (q + 1) & kmask;
        int32_t i = s->keys[q].head == -2 ? -1 : s->keys[q].head;
        if (i < 0) {
            unmatched += bd->count;
            continue;
        }
        s->keys[q].head = s->next[i];
        matched_draws++;
        const ip_draw * ad = &A->d[i];
        const ip_vert * av = A->v + ad->first, * bv = B->v + bd->first;
        if (ad->mode == bd->mode && ad->count == bd->count && ip_same_uv(av, bv, bd->count)) {
            for (uint32_t v = 0; v < bd->count; v++) ip_moved(av + v, bv + v, &p);
            fast += bd->count;
        } else if (ad->mode == IP_TRIANGLES && bd->mode == IP_TRIANGLES) {
            int64_t m = ip_match_tris(av, ad->count, bv, bd->count, s, &p);
            if (m < 0) m = 0;
            by_uv += (uint64_t) m;
            unmatched += bd->count - (uint64_t) m;
        } else {
            unmatched += bd->count;
        }
    }
    st->pairs++;
    st->draws += B->nd;
    st->draws_matched += matched_draws;
    st->verts += B->nv;
    st->fast += fast;
    st->by_uv += by_uv;
    st->unmatched += unmatched;
    if (p.moving > 100 && 2 * p.far > p.moving) {
        st->cuts++;
        return;
    }
    st->moving += p.moving;
    st->far += p.far;
    st->dsum += p.dsum;
    if (p.dmax > st->dmax) st->dmax = p.dmax;
}

#endif // FF7_INTERP_CORE_H
