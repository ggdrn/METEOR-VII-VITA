/*
 * PlayStation button symbols instead of the Xbox letters, matching the
 * physical mapping of the port: A -> Cross, B -> Circle, X -> Square,
 * Y -> Triangle. Nothing is shipped: every symbol is drawn at run time.
 *
 * Button atlas: assets/button_*.png (loaded through MainActivity.ImageGetData)
 * labels the touch buttons with letters. When the atlas is loaded, the
 * letters inside the A/B/X/Y circles are erased and the symbols drawn in
 * their place. Circle positions come from the v1.0.29 atlas (1024x1024);
 * each circle is only touched if its outline is found where expected.
 *
 * Text: dialogs and world map help say "(A)", "(B)", "(X)", "(Y)" as plain
 * text in the menu font, after a color code ({FE}{D6}(A){FE}{D9}). The font
 * has no symbols, so (ff7_buttons_core.h):
 *  - font: when usfont_a_h/_l.tex goes through convert_image_data, the cells
 *    of characters 0x96-0x99 (unused math symbols) are redrawn with the
 *    four symbols, in the font's own indices (checked by hashing the
 *    original cells first);
 *  - widths: their entries in the width table (pointer at 0xDB958C, filled
 *    from window.bin) get the symbols' advance;
 *  - strings: field files, right after the LZSS decompressor (0x6305C0)
 *    unpacks them, and the world map "mes" table (loaded by 0x74E1E9 into
 *    0xE36120) have {FE}{Dx}(A) replaced by {FE}{Dy}<symbol>, in place,
 *    with the symbol's color. Only once the font has the symbols.
 *
 * DATA_PATH "psbuttons_off.txt" keeps everything original.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_buttons.h"
#include "ff7_buttons_core.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <math.h>
#include <string.h>

#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH DATA_PATH "psbuttons_off.txt"

#define SYM_MK_ADDRESS "mk_address"
#define SYM_WM_INIT    "_Z15func_0x0074e1e9v"
#define WM_INIT_OFFSET 0x763ae4

#define WIDTH_TABLE_PTR 0xdb958c // u32: x86 address of the font width table
#define WM_MES          0xe36120 // world map messages: u16 count, u16 offsets[], strings
#define WM_MES_SIZE     0x1000   // as loaded by 0x74E1E9

// FNV-1a of the cells of 0x96..0x99 in the original fonts (v1.0.29, every menu_*.lgp)
#define FONT_HASH_H 0x47852591u // usfont_a_h.tex
#define FONT_HASH_L 0x506f09b0u // usfont_a_l.tex

#define FIELD_SECTIONS 9
#define FIELD_SECTION0 0x2a
#define MAX_STRING     4096

typedef uint8_t * (* mk_address_fn)(uint32_t addr);

static int enabled;      // atlas and text (no psbuttons_off.txt)
static int text_enabled; // text: needs mk_address
static mk_address_fn mk_address;
static so_hook wm_init_hook;
static uint32_t painted_hash; // cells hash once the symbols are drawn
static int font_ready;        // the font has the symbols: strings may use them
static unsigned fields_patched, labels_patched;

// Translation cache for one stream of x86 addresses (pages need not be contiguous)
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

static uint32_t rd16(xcache * c, uint32_t a) {
    return *xptr(c, a) | (*xptr(c, a + 1) << 8);
}

static uint32_t rd32(xcache * c, uint32_t a) {
    return rd16(c, a) | (rd16(c, a + 2) << 16);
}

// --- Font ---

static uint32_t cells_hash(uint32_t image) {
    xcache c = { 0xffffffff, NULL };
    uint32_t h = PSB_FNV_INIT;
    for (int i = 0; i < PSB_COUNT; i++) {
        int x0, y0;
        psb_cell_origin(PSB_FIRST_CHAR + i, &x0, &y0);
        for (int y = 0; y < PSB_CELL; y++) {
            // A cell row never crosses a page: rows are 256 bytes
            const uint8_t * row = xptr(&c, image + (y0 + y) * 256 + x0);
            for (int x = 0; x < PSB_CELL; x++) h = psb_fnv(h, row[x]);
        }
    }
    return h;
}

void ff7_buttons_font(const void * th) {
    if (!text_enabled || !th) return;
    const uint8_t * t = th;
    // t_ff7_tex_header: width +0x3c, height +0x40, bytes per pixel +0x68, indices +0xd4
    if (*(const int32_t *) (t + 0x3c) != 256 || *(const int32_t *) (t + 0x40) != 256
        || *(const int32_t *) (t + 0x68) != 1)
        return;
    uint32_t image = *(const uint32_t *) (t + 0xd4);
    if (!image) return;

    uint32_t h = cells_hash(image);
    if (painted_hash && h == painted_hash) return; // already drawn
    if (h != FONT_HASH_H && h != FONT_HASH_L) return;

    xcache c = { 0xffffffff, NULL };
    uint8_t cell[PSB_CELL][PSB_CELL];
    for (int i = 0; i < PSB_COUNT; i++) {
        int x0, y0;
        psb_cell_origin(PSB_FIRST_CHAR + i, &x0, &y0);
        psb_draw_cell(i, cell);
        for (int y = 0; y < PSB_CELL; y++)
            memcpy(xptr(&c, image + (y0 + y) * 256 + x0), cell[y], PSB_CELL);
    }
    painted_hash = cells_hash(image);
    font_ready = 1;
    l_info("ff7_buttons: PlayStation symbols drawn in the %s font", h == FONT_HASH_H ? "_h" : "_l");
}

// Advance of the symbol characters (window.bin may be reloaded: checked every time)
static void patch_widths(void) {
    xcache c = { 0xffffffff, NULL };
    uint32_t table = rd32(&c, WIDTH_TABLE_PTR);
    if (!table) return;
    for (int i = 0; i < PSB_COUNT; i++) {
        uint8_t * w = xptr(&c, table + PSB_FIRST_CHAR + i);
        if (*w == psb_orig_width[i]) *w = PSB_WIDTH;
    }
}

// --- Strings ---

// Replaces the labels of the string at `addr` (at most `max` bytes, terminator included)
static int patch_string(uint32_t addr, uint32_t max) {
    static uint8_t buf[MAX_STRING]; // game thread only
    xcache c = { 0xffffffff, NULL };
    if (max > MAX_STRING) max = MAX_STRING;
    uint32_t len = 0;
    while (len < max) {
        buf[len] = *xptr(&c, addr + len);
        if (buf[len++] == FF7_END) break;
    }
    int n = psb_replace(buf, len);
    if (n <= 0) return 0;
    for (uint32_t i = 0; i < len; i++) *xptr(&c, addr + i) = buf[i];
    return n;
}

// Strings table: u16 count, u16 offsets[count] (from the table), within `size` bytes
static int patch_table(uint32_t table, uint32_t size) {
    xcache c = { 0xffffffff, NULL };
    if (size < 2) return 0;
    uint32_t count = rd16(&c, table);
    if (count == 0 || 2 + 2 * count > size) return 0;
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd16(&c, table + 2 + 2 * i);
        if (off < 2 + 2 * count || off >= size) continue;
        n += patch_string(table + off, size - off);
    }
    return n;
}

void ff7_buttons_lzss(uint32_t dst) {
    if (!text_enabled || !font_ready) return;
    xcache c = { 0xffffffff, NULL };
    // Field file: u16 0, u32 9 sections, u32 offsets (the first at 0x2a);
    // section 0 = u32 size + script: u16 version, u8, u8, u16 strings table offset
    if (rd16(&c, dst) != 0 || rd32(&c, dst + 2) != FIELD_SECTIONS || rd32(&c, dst + 6) != FIELD_SECTION0)
        return;
    uint32_t size = rd32(&c, dst + FIELD_SECTION0);
    uint32_t script = dst + FIELD_SECTION0 + 4;
    if (size < 8 || size > 0x100000 || rd16(&c, script) != 0x0502) return;
    uint32_t strings = rd16(&c, script + 4);
    if (strings >= size) return;

    patch_widths();
    int n = patch_table(script + strings, size - strings);
    fields_patched++;
    labels_patched += n;
    if (n) l_info("ff7_buttons: field text: %d button labels -> PlayStation symbols", n);
}

static void wm_init(void) {
    SO_CONTINUE(int, wm_init_hook);
    if (!font_ready) return;
    patch_widths();
    int n = patch_table(WM_MES, WM_MES_SIZE);
    if (n) l_info("ff7_buttons: world map text: %d button labels -> PlayStation symbols", n);
}

void ff7_buttons_patch(void) {
    enabled = !file_exists(OFF_SWITCH);
    if (!enabled) {
        l_info("ff7_buttons: %s found, Xbox button labels kept", OFF_SWITCH);
        return;
    }
    mk_address = (mk_address_fn) so_symbol(&so_mod, SYM_MK_ADDRESS);
    if (!mk_address) {
        // The atlas symbols do not need it; the text ones do
        l_warn("ff7_buttons: no mk_address, text symbols disabled");
        return;
    }
    text_enabled = 1;
    uintptr_t wm = so_symbol(&so_mod, SYM_WM_INIT);
    if (wm && (wm & ~1) == so_mod.text_base + WM_INIT_OFFSET)
        wm_init_hook = hook_addr(wm, (uintptr_t) &wm_init);
    else
        l_warn("ff7_buttons: unexpected .so layout, world map text left as is");
    l_info("ff7_buttons: PlayStation button symbols on (%s disables them)", OFF_SWITCH);
}

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
    if (!enabled || !path || !strstr(path, "button_") || w != 1024 || h != 1024) return 0;

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
