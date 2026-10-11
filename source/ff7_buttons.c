/*
 * PlayStation button glyphs on the game's button atlas.
 *
 * assets/button_*.png (loaded through MainActivity.ImageGetData) labels the
 * buttons with Xbox-style letters. When the atlas is loaded, the letters
 * inside the A/B/X/Y circles are erased and PlayStation symbols are drawn in
 * their place, procedurally, matching the physical mapping of the port:
 * A -> Cross, B -> Circle, X -> Square, Y -> Triangle.
 *
 * Circle positions come from the v1.0.29 atlas (1024x1024); each circle is
 * only touched if its outline is found where expected.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_buttons.h"

#include "utils/logger.h"

#include <math.h>
#include <string.h>

enum glyph { CROSS, CIRCLE, SQUARE, TRIANGLE };

static const struct {
    int cx, cy, r;   // circle center and outer radius
    enum glyph g;
} buttons[] = {
    // small cluster (left)
    { 46, 633, 46, CROSS },     // A
    { 114, 610, 22, CIRCLE },   // B
    { 161, 610, 22, SQUARE },   // X
    { 207, 610, 22, TRIANGLE }, // Y
    // large cluster (right)
    { 585, 467, 46, CIRCLE },   // B
    { 681, 467, 46, SQUARE },   // X
    { 777, 467, 46, TRIANGLE }, // Y
};

#define RING 3 // outline thickness in the atlas

static inline uint32_t argb_alpha(uint32_t p) { return p >> 24; }

// Distance from p to segment ab
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

// Distance from (x, y) (relative to the center) to the glyph outline
static float glyph_dist(enum glyph g, float x, float y, float r) {
    switch (g) {
        case CROSS: {
            float s = r * 0.40f;
            float d1 = seg_dist(x, y, -s, -s, s, s);
            float d2 = seg_dist(x, y, -s, s, s, -s);
            return d1 < d2 ? d1 : d2;
        }
        case CIRCLE:
            return fabsf(sqrtf(x * x + y * y) - r * 0.42f);
        case SQUARE: {
            float s = r * 0.38f, d = 1e9f, t;
            t = seg_dist(x, y, -s, -s, s, -s); if (t < d) d = t;
            t = seg_dist(x, y, s, -s, s, s);   if (t < d) d = t;
            t = seg_dist(x, y, s, s, -s, s);   if (t < d) d = t;
            t = seg_dist(x, y, -s, s, -s, -s); if (t < d) d = t;
            return d;
        }
        case TRIANGLE: default: {
            float ax = 0, ay = -r * 0.46f;
            float bx = -r * 0.46f, by = r * 0.34f;
            float cx = r * 0.46f, cy = r * 0.34f, d, t;
            d = seg_dist(x, y, ax, ay, bx, by);
            t = seg_dist(x, y, bx, by, cx, cy); if (t < d) d = t;
            t = seg_dist(x, y, cx, cy, ax, ay); if (t < d) d = t;
            return d;
        }
    }
}

int ff7_buttons_patch_atlas(const char * path, uint32_t * argb, int w, int h) {
    if (!path || !strstr(path, "button_") || w != 1024 || h != 1024) return 0;

    int patched = 0;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        int cx = buttons[i].cx, cy = buttons[i].cy, r = buttons[i].r;
        // The outline must be where expected (left and right edge of the ring)
        if (argb_alpha(argb[cy * w + (cx - r + 1)]) < 128 || argb_alpha(argb[cy * w + (cx + r - 1)]) < 128) {
            l_warn("ff7_buttons: %s: no outline at (%d,%d), skipped", path, cx, cy);
            continue;
        }
        uint32_t color = argb[cy * w + (cx - r + 1)] & 0x00ffffff;
        float half_width = r * 0.055f + 0.8f;
        float inner = r - RING - 1.0f;

        for (int y = cy - r; y <= cy + r; y++) {
            for (int x = cx - r; x <= cx + r; x++) {
                float dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > inner * inner) continue; // keep the outline
                float d = glyph_dist(buttons[i].g, dx, dy, (float) r);
                float a = half_width + 0.5f - d; // 1px anti-aliased edge
                a = a < 0 ? 0 : a > 1 ? 1 : a;
                argb[y * w + x] = a > 0 ? ((uint32_t) (a * 255.f) << 24) | color : 0;
            }
        }
        patched++;
    }
    if (patched) l_info("ff7_buttons: %s: %d PlayStation glyphs drawn", path, patched);
    return patched;
}
