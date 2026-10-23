/*
 * PlayStation symbols for the game's text: pure logic shared by
 * ff7_buttons.c and the host test (no Vita or emulated-memory dependency).
 *
 * Glyphs: the menu font (usfont_a_h.tex, 256x256, 8-bit indices) has 24x24
 * cells, 21 per row across usfont_a_h + usfont_b_h; character c is in column
 * c % 21, row c / 21. Index 3 is the letter body, index 1 its shadow (the
 * body dilated 0-2 px right and down), 0 transparent; the text color comes
 * from the renderer, so a symbol drawn with these indices takes every color.
 * Characters 0x96-0x99 (math symbols no FF7 text uses) get the four symbols.
 *
 * Text: the Xbox labels are plain text, always after a color code:
 * {FE}{Dx}(A). They become {FE}{Dy}<symbol>, two bytes shorter, with the
 * PlayStation color of the symbol.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_BUTTONS_CORE_H
#define FF7_BUTTONS_CORE_H

#include <math.h>
#include <stdint.h>

#define PSB_CELL       24
#define PSB_PER_ROW    21
#define PSB_FIRST_CHAR 0x96  // Cross, Circle, Square, Triangle: 0x96..0x99
#define PSB_COUNT      4
#define PSB_WIDTH      10    // width table entry: advance of 10 * 2 px, no left pad

// Width table entries of 0x96..0x99 in window.bin (v1.0.29), checked before patching
static const uint8_t psb_orig_width[PSB_COUNT] = { 0x07, 0x07, 0x09, 0x08 };

enum { PSB_CROSS, PSB_CIRCLE, PSB_SQUARE, PSB_TRIANGLE };

// FF7 text encoding: character = ASCII - 0x20
#define FF7_LPAREN 0x08
#define FF7_RPAREN 0x09
#define FF7_COLOR  0xFE // followed by 0xD2..0xD9
#define FF7_END    0xFF

// Label letter -> symbol, and the color code it gets (D3 blue, D4 red, D5 purple, D6 green)
static const struct {
    uint8_t letter;
    uint8_t glyph;
    uint8_t color;
} psb_map[PSB_COUNT] = {
    { 'A' - 0x20, PSB_FIRST_CHAR + PSB_CROSS,    0xD3 },
    { 'B' - 0x20, PSB_FIRST_CHAR + PSB_CIRCLE,   0xD4 },
    { 'X' - 0x20, PSB_FIRST_CHAR + PSB_SQUARE,   0xD5 },
    { 'Y' - 0x20, PSB_FIRST_CHAR + PSB_TRIANGLE, 0xD6 },
};

static inline float psb_seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

// Distance from (x, y), relative to the symbol center, to its outline
static inline float psb_dist(int g, float x, float y) {
    float d, t;
    switch (g) {
        case PSB_CROSS:
            d = psb_seg_dist(x, y, -5.f, -5.f, 5.f, 5.f);
            t = psb_seg_dist(x, y, -5.f, 5.f, 5.f, -5.f);
            return (d < t ? d : t) + 0.15f; // diagonals: a bit thinner
        case PSB_CIRCLE:
            return fabsf(sqrtf(x * x + y * y) - 5.8f);
        case PSB_SQUARE:
            // Straight edges on whole pixels: 2 px thick, like the letters
            d = psb_seg_dist(x, y, -6.f, -6.f, 6.f, -6.f);
            t = psb_seg_dist(x, y, 6.f, -6.f, 6.f, 6.f);   if (t < d) d = t;
            t = psb_seg_dist(x, y, 6.f, 6.f, -6.f, 6.f);   if (t < d) d = t;
            t = psb_seg_dist(x, y, -6.f, 6.f, -6.f, -6.f); if (t < d) d = t;
            return d;
        default: // PSB_TRIANGLE
            d = psb_seg_dist(x, y, 0.f, -6.5f, -6.5f, 6.f);
            t = psb_seg_dist(x, y, -6.5f, 6.f, 6.5f, 6.f); if (t < d) d = t;
            t = psb_seg_dist(x, y, 6.5f, 6.f, 0.f, -6.5f); if (t < d) d = t;
            return d;
    }
}

/** Fills a 24x24 cell with font indices (0, 1 shadow, 3 body) for symbol g. */
static inline void psb_draw_cell(int g, uint8_t cell[PSB_CELL][PSB_CELL]) {
    uint8_t body[PSB_CELL][PSB_CELL];
    // Center at (8, 10): body in columns 1-15 and rows 3-17, like the capitals
    for (int y = 0; y < PSB_CELL; y++)
        for (int x = 0; x < PSB_CELL; x++)
            body[y][x] = psb_dist(g, x + 0.5f - 8.f, y + 0.5f - 10.f) <= 1.0f;
    for (int y = 0; y < PSB_CELL; y++) {
        for (int x = 0; x < PSB_CELL; x++) {
            uint8_t v = 0;
            if (body[y][x]) {
                v = 3;
            } else {
                for (int dy = 0; dy <= 2 && !v; dy++)
                    for (int dx = 0; dx <= 2 && !v; dx++)
                        if (y >= dy && x >= dx && body[y - dy][x - dx]) v = 1;
            }
            cell[y][x] = v;
        }
    }
}

/** Top-left pixel of character c's cell in usfont_a_h (all PSB chars are there). */
static inline void psb_cell_origin(int c, int * x, int * y) {
    *x = (c % PSB_PER_ROW) * PSB_CELL;
    *y = (c / PSB_PER_ROW) * PSB_CELL;
}

/** FNV-1a, fed one byte at a time. */
static inline uint32_t psb_fnv(uint32_t h, uint8_t b) {
    return (h ^ b) * 16777619u;
}
#define PSB_FNV_INIT 2166136261u

/**
 * Replaces the labels in one string, in place. `s` holds the string and its
 * FF7_END terminator within `len` bytes. Each replacement shortens it by two;
 * the freed bytes at the end become FF7_END. Returns the labels replaced, or
 * -1 if there is no terminator (not a string).
 */
static inline int psb_replace(uint8_t * s, uint32_t len) {
    uint32_t end = 0;
    while (end < len && s[end] != FF7_END) end++;
    if (end == len) return -1;
    int n = 0;
    for (uint32_t i = 0; i + 5 <= end; i++) {
        if (s[i] != FF7_COLOR || s[i + 1] < 0xD2 || s[i + 1] > 0xD9) continue;
        if (s[i + 2] != FF7_LPAREN || s[i + 4] != FF7_RPAREN) continue;
        int k = 0;
        while (k < PSB_COUNT && psb_map[k].letter != s[i + 3]) k++;
        if (k == PSB_COUNT) continue;
        s[i + 1] = psb_map[k].color;
        s[i + 2] = psb_map[k].glyph;
        for (uint32_t j = i + 3; j + 2 <= end; j++) s[j] = s[j + 2];
        s[end - 1] = s[end] = FF7_END;
        end -= 2;
        n++;
    }
    return n;
}

#endif // FF7_BUTTONS_CORE_H
