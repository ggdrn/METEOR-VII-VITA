/*
 * Host test of source/ff7_interp_core.h (battle at 30 fps): synthetic pairs
 * of frames whose answer is known.
 *  - identical frames: everything in the fast case, nothing moves;
 *  - a model moved by (3, 4): fast case, every vertex lands halfway
 *    (x + 1.5, y + 2, z unchanged), 5 px measured on every 4th vertex;
 *  - its triangles shuffled and one dropped: paired by u/v, halfway to their
 *    own pair (the map follows the u/v, not the order);
 *  - repeated u/v (untextured), count changed: paired in order;
 *  - a draw whose state key is new: no pair, left as in B;
 *  - same key and count but another mesh: no pair;
 *  - draws with the same key paired in order, not by position in the frame;
 *  - a triangle with a vertex thrown 500 px away stays as in B;
 *  - a frame where everything jumps 300 px: one cut;
 *  - buffers too small: one overflow, nothing counted.
 *
 *   cc -O2 -I source scripts/test_interp.c -o /tmp/ti -lm && /tmp/ti
 */
#include "ff7_interp_core.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t rng = 777;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct {
    float x, y, z;
    uint32_t rgba;
    float u, v;
} vtx; // the game's 24-byte vertex

#define MAXV 4096
static vtx model[MAXV], work[MAXV], flat[90], flat_b[90];
static ip_draw da[64], db[64];
static ip_keyslot keys[256];
static int32_t next_[64];
static ip_trislot tris[8192];
static int32_t map[MAXV * 4];
static ip_frame A_, B_;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void make_model(vtx * m, int n, int textured) {
    for (int i = 0; i < n; i++) {
        m[i].x = (float) (rnd() % 640);
        m[i].y = (float) (rnd() % 480);
        m[i].z = (float) (rnd() % 1000) / 1000.0f;
        m[i].rgba = rnd();
        m[i].u = textured ? (float) (rnd() % 4096) / 4096.0f : 0.0f;
        m[i].v = textured ? (float) (rnd() % 4096) / 4096.0f : 0.0f;
    }
}

static void add(ip_frame * f, uint32_t key, const vtx * v, int n) {
    ip_add(f, key, IP_TRIANGLES, (uint32_t) n, (const uint8_t *) &v[0].x, sizeof(vtx), 1, (const uint8_t *) &v[0].u,
           sizeof(vtx), 0);
}

static ip_stats run(void (* build)(ip_frame *, ip_frame *)) {
    ip_frame A = { da, 0, 0, 64, MAXV * 4, 0 }, B = { db, 0, 0, 64, MAXV * 4, 0 };
    ip_scratch s = { keys, next_, tris, 256, 8192 };
    ip_stats st;
    memset(&st, 0, sizeof(st));
    build(&A, &B);
    ip_match(&A, &B, &s, &st, map);
    A_ = A;
    B_ = B;
    return st;
}

// ip_mid over B's draw d, on a copy of its vertices as the game's 24-byte records
static vtx mid[MAXV];
static uint32_t mid_of(int d) {
    const ip_draw * bd = &B_.d[d];
    for (uint32_t i = 0; i < bd->count; i++) ip_xyz(bd, i, &mid[i].x);
    return ip_mid(&A_, bd, map, (uint8_t *) &mid[0].x, sizeof(vtx));
}

static void b_same(ip_frame * A, ip_frame * B) {
    add(A, 1, model, 300);
    add(A, 2, model + 300, 60);
    add(B, 1, model, 300);
    add(B, 2, model + 300, 60);
}

static void b_moved(ip_frame * A, ip_frame * B) {
    add(A, 1, model, 300);
    add(A, 2, model + 300, 60);
    memcpy(work, model, 300 * sizeof(vtx));
    for (int i = 0; i < 300; i++) work[i].x += 3, work[i].y += 4;
    add(B, 1, work, 300);
    add(B, 2, model + 300, 60);
}

static void b_shuffled(ip_frame * A, ip_frame * B) {
    add(A, 1, model, 300);
    // B: triangles in reverse order, the last one of A dropped, moved by (6, 8)
    int n = 0;
    for (int t = 98; t >= 0; t--)
        for (int k = 0; k < 3; k++) {
            work[n] = model[3 * t + k];
            work[n].x += 6;
            work[n].y += 8;
            n++;
        }
    add(B, 1, work, n);
}

static void b_untextured(ip_frame * A, ip_frame * B) {
    make_model(flat, 90, 0);
    add(A, 7, flat, 90);
    memcpy(flat_b, flat, sizeof(flat));
    for (int i = 0; i < 90; i++) flat_b[i].x += 1;
    add(B, 7, flat_b, 87); // count changes: 87 vertices
}

static void b_newkey(ip_frame * A, ip_frame * B) {
    add(A, 1, model, 300);
    add(B, 1, model, 300);
    add(B, 9, model + 300, 30);
}

static void b_order(ip_frame * A, ip_frame * B) {
    // two draws with key 3 around one with key 4; B has the key-4 draw first
    add(A, 3, model, 30);
    add(A, 4, model + 30, 30);
    add(A, 3, model + 60, 30);
    add(B, 4, model + 30, 30);
    add(B, 3, model, 30);
    add(B, 3, model + 60, 30);
}

static void b_other_mesh(ip_frame * A, ip_frame * B) {
    // same key and count, another mesh (other u/v): no vertex may pair
    add(A, 5, model, 30);
    add(B, 5, model + 30, 30);
}

static void b_thrown(ip_frame * A, ip_frame * B) {
    // moved by (3, 4), but vertex 31 (triangle 10) is thrown 500 px away
    add(A, 1, model, 300);
    memcpy(work, model, 300 * sizeof(vtx));
    for (int i = 0; i < 300; i++) work[i].x += 3, work[i].y += 4;
    work[31].x += 500;
    add(B, 1, work, 300);
}

static void b_cut(ip_frame * A, ip_frame * B) {
    add(A, 1, model, 300);
    memcpy(work, model, 300 * sizeof(vtx));
    for (int i = 0; i < 300; i++) work[i].x += 300;
    add(B, 1, work, 300);
}

static void b_overflow(ip_frame * A, ip_frame * B) {
    A->maxv = 100;
    add(A, 1, model, 300);
    add(B, 1, model, 300);
}

int main(void) {
    make_model(model, 1000, 1);
    ip_stats s;

    s = run(b_same);
    CHECK(s.pairs == 1 && s.draws == 2 && s.draws_matched == 2 && s.verts == 360, "same: counts");
    CHECK(s.fast == 360 && s.by_uv == 0 && s.unmatched == 0 && s.moving == 0, "same: fast %llu moving %llu",
          (unsigned long long) s.fast, (unsigned long long) s.moving);
    CHECK(B_.d[0].kind == IP_FAST && B_.d[0].pair == 0 && B_.d[1].pair == 1, "same: kinds");

    s = run(b_moved);
    CHECK(s.fast == 360 && s.moving == 75 && s.sampled == 90, "moved: fast %llu moving %llu sampled %llu",
          (unsigned long long) s.fast, (unsigned long long) s.moving, (unsigned long long) s.sampled);
    CHECK(fabs(s.dsum / s.moving - 5.0) < 1e-4 && fabsf(s.dmax - 5.0f) < 1e-4f, "moved: mean %f max %f",
          s.dsum / s.moving, s.dmax);
    {
        uint32_t moved = mid_of(0);
        int bad = 0;
        for (int i = 0; i < 300; i++)
            bad += fabsf(mid[i].x - (model[i].x + 1.5f)) > 1e-3f || fabsf(mid[i].y - (model[i].y + 2.0f)) > 1e-3f
                   || fabsf(mid[i].z - model[i].z) > 1e-6f;
        CHECK(moved == 300 && !bad, "moved: halfway %u, %d wrong", moved, bad);
    }

    s = run(b_shuffled);
    CHECK(s.fast == 0 && s.by_uv == 297 && s.unmatched == 0 && s.moving == 297, "shuffled: uv %llu unmatched %llu "
          "moving %llu", (unsigned long long) s.by_uv, (unsigned long long) s.unmatched, (unsigned long long) s.moving);
    CHECK(fabs(s.dsum / s.moving - 10.0) < 1e-4, "shuffled: mean %f", s.dsum / s.moving);
    {
        // B's triangle t (reversed) is A's triangle 98 - t, moved by (6, 8): halfway is A + (3, 4)
        uint32_t moved = mid_of(0);
        int bad = 0;
        for (int t = 0; t < 99; t++)
            for (int k = 0; k < 3; k++) {
                const vtx * a = &model[3 * (98 - t) + k];
                bad += fabsf(mid[3 * t + k].x - (a->x + 3.0f)) > 1e-3f || fabsf(mid[3 * t + k].y - (a->y + 4.0f)) > 1e-3f;
            }
        CHECK(moved == 297 && !bad, "shuffled: halfway %u, %d wrong", moved, bad);
    }

    s = run(b_untextured);
    // 29 triangles in B, all with u/v 0: paired in order with A's first 29
    CHECK(s.by_uv == 87 && s.unmatched == 0, "untextured: uv %llu unmatched %llu", (unsigned long long) s.by_uv,
          (unsigned long long) s.unmatched);
    {
        uint32_t moved = mid_of(0);
        int bad = 0;
        for (int i = 0; i < 87; i++) bad += fabsf(mid[i].x - (flat[i].x + 0.5f)) > 1e-3f;
        CHECK(moved == 87 && !bad, "untextured: halfway %u, %d wrong", moved, bad);
    }

    s = run(b_newkey);
    CHECK(s.draws == 2 && s.draws_matched == 1 && s.fast == 300 && s.unmatched == 30, "new key: matched %llu "
          "unmatched %llu", (unsigned long long) s.draws_matched, (unsigned long long) s.unmatched);
    {
        uint32_t moved = mid_of(1);
        int bad = 0;
        for (int i = 0; i < 30; i++) bad += mid[i].x != model[300 + i].x || mid[i].y != model[300 + i].y;
        CHECK(moved == 0 && !bad, "new key: %u moved, %d changed", moved, bad);
    }

    s = run(b_order);
    CHECK(s.draws_matched == 3 && s.fast == 90 && s.unmatched == 0, "order: fast %llu", (unsigned long long) s.fast);
    CHECK(B_.d[0].pair == 1 && B_.d[1].pair == 0 && B_.d[2].pair == 2, "order: pairs %d %d %d", B_.d[0].pair,
          B_.d[1].pair, B_.d[2].pair);

    s = run(b_other_mesh);
    CHECK(s.fast == 0 && s.by_uv == 0 && s.unmatched == 30 && mid_of(0) == 0, "other mesh: fast %llu uv %llu",
          (unsigned long long) s.fast, (unsigned long long) s.by_uv);

    s = run(b_thrown);
    {
        uint32_t moved = mid_of(0);
        int bad = 0;
        for (int i = 0; i < 300; i++) {
            int in_thrown = i / 3 == 10;
            float ex = in_thrown ? work[i].x : model[i].x + 1.5f, ey = in_thrown ? work[i].y : model[i].y + 2.0f;
            bad += fabsf(mid[i].x - ex) > 1e-3f || fabsf(mid[i].y - ey) > 1e-3f;
        }
        CHECK(moved == 297 && !bad, "thrown: %u moved, %d wrong", moved, bad);
    }

    s = run(b_cut);
    CHECK(s.cuts == 1 && s.moving == 0 && s.dmax == 0.0f && s.fast == 300, "cut: cuts %u moving %llu", s.cuts,
          (unsigned long long) s.moving);

    s = run(b_overflow);
    CHECK(s.overflows == 1 && s.pairs == 0 && s.verts == 0, "overflow: %u", s.overflows);
    CHECK(B_.d[0].kind == IP_NONE && mid_of(0) == 0, "overflow: nothing moves");

    printf(fails ? "%d FAILED\n" : "all interpolation tests passed\n", fails);
    return fails != 0;
}
