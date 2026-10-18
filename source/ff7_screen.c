/*
 * Field mode zoom of the final picture for libjni_ff7.so.
 *
 * The game draws each frame into a 640x480 offscreen texture. VIDEO_update
 * (on the main thread) then draws it on the screen as one quad as tall as the
 * screen (725x544 on the Vita, centered), followed by the side borders and the
 * touch UI. In field mode the game leaves the last 16 of the 480 lines empty,
 * which showed as a black strip at the bottom of the screen (18 px).
 *
 * While the field background renderer runs (ff7_bg_last_us), the frame's quad
 * is scaled by 480/464 around the top center: the field fills the screen
 * height (750 px wide), the empty lines fall below the screen, and the
 * picture keeps its aspect. The side borders are shortened to match. The UI
 * ("Encounters ON", "A") is drawn afterwards in its usual place, on top.
 * Battle, menus, movies and the world map are drawn as before.
 *
 * Both quads are patched right after the game builds them:
 *   VIDEO_doQuad (6 vertices: x, y, z, color, u, v; 0x18 bytes each), then
 *   VIDEO_doQuad_tex_forOffscreen (texture coordinates of the frame quad) or
 *   VIDEO_doQuad_color_forBorder (colors of a border quad), before glEnd.
 * Only the calls from VIDEO_update are touched (checked by return address).
 *
 * gfx_setviewport is hooked to log each distinct drawing area the game asks
 * for, with the mode, to check the 464 lines in other fields.
 *
 * Creating DATA_PATH "field_zoom_off.txt" turns the zoom off.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_screen.h"
#include "ff7_bg.h"

#include "utils/logger.h"
#include "utils/utils.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

extern so_module so_mod;

#define OFF_SWITCH       DATA_PATH "field_zoom_off.txt"
#define SYM_QUAD_TEX     "VIDEO_doQuad_tex_forOffscreen"
#define SYM_BORDER       "VIDEO_doQuad_color_forBorder"
#define SYM_VIEWPORT     "_Z15gfx_setviewportjjjjj"
#define QUAD_TEX_PROLOGUE 0xee07b410 // push {r4}; vmov s14, r0
#define BORDER_PROLOGUE   0x25ffb4f0 // push {r4-r7}; movs r5, #255
#define VIEWPORT_PROLOGUE 0x41f0e92d // push.w {r4-r8, lr}
#define CUR_VERTEX_GOT   0xf4c984   // GOT slot read by the quad helpers
#define CUR_VERTEX       0xf4f4d8   // pCurVertex
// Return addresses (Thumb) of the calls from VIDEO_update
#define RET_FRAME_QUAD   0x953771
#define RET_BORDER_LEFT  0x9537b3
#define RET_BORDER_RIGHT 0x9537db

#define FRAME_LINES      480.0f
#define FIELD_LINES      464.0f     // lines the field fills (screenshot: 18 of 544 px empty)
#define FIELD_TIMEOUT_US 250000     // field mode ends this long after the last background frame
#define VERTEX_SIZE      0x18
#define QUAD_VERTICES    6
#define MAX_VIEWPORTS    32

static so_hook quad_tex_hook, border_hook, viewport_hook;
static uint8_t ** cur_vertex;
static uintptr_t base;
static int zoom_enabled, zoomed;
// Frame quad of the current VIDEO_update, after the zoom (for the borders)
static float zoom_left, zoom_right, zoom_center;

static struct { uint32_t x, y, w, h; int field; } viewports[MAX_VIEWPORTS];
static int viewport_count;

static int in_field(void) {
    uint64_t last = ff7_bg_last_us;
    return last && sceKernelGetProcessTimeWide() - last < FIELD_TIMEOUT_US;
}

static inline float * vertex(uint8_t * end, int i) {
    return (float *) (end - (QUAD_VERTICES - i) * VERTEX_SIZE);
}

// Float arguments in core registers (softfp), passed on as raw bits:
// SO_CONTINUE calls through an unprototyped pointer, which would widen floats
static int quad_tex_hooked(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    int r = SO_CONTINUE(int, quad_tex_hook, x, y, w, h);
    if ((uintptr_t) __builtin_return_address(0) != base + RET_FRAME_QUAD) return r;

    int field = zoom_enabled && in_field();
    if (field != zoomed) {
        zoomed = field;
        l_info("field zoom: %s", field ? "on" : "off");
    }
    if (!field) {
        zoom_left = zoom_right = 0;
        return r;
    }

    // The quad VIDEO_doQuad just wrote: x0..x1 by 0..screen height (y down)
    uint8_t * end = *cur_vertex;
    float x0 = vertex(end, 0)[0], x1 = vertex(end, 1)[0];
    const float k = FRAME_LINES / FIELD_LINES;
    zoom_center = (x0 + x1) * 0.5f;
    zoom_left = zoom_center - (zoom_center - x0) * k;
    zoom_right = zoom_center + (x1 - zoom_center) * k;
    for (int i = 0; i < QUAD_VERTICES; i++) {
        float * v = vertex(end, i);
        v[0] = zoom_center + (v[0] - zoom_center) * k;
        v[1] *= k; // top stays at 0, the empty lines go below the screen
    }
    return r;
}

static int border_hooked(int r0, int g0, int b0, int r1, int g1, int b1) {
    int r = SO_CONTINUE(int, border_hook, r0, g0, b0, r1, g1, b1);
    uintptr_t ret = (uintptr_t) __builtin_return_address(0);
    if (!zoomed || zoom_left == zoom_right || (ret != base + RET_BORDER_LEFT && ret != base + RET_BORDER_RIGHT))
        return r;

    // Pull the inner edge of the border out to the zoomed frame's edge
    uint8_t * end = *cur_vertex;
    for (int i = 0; i < QUAD_VERTICES; i++) {
        float * v = vertex(end, i);
        if (v[0] > zoom_left && v[0] < zoom_center) v[0] = zoom_left;
        else if (v[0] < zoom_right && v[0] >= zoom_center) v[0] = zoom_right;
    }
    return r;
}

static int viewport_hooked(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t arg) {
    int field = in_field();
    int i;
    for (i = 0; i < viewport_count; i++)
        if (viewports[i].x == x && viewports[i].y == y && viewports[i].w == w && viewports[i].h == h
            && viewports[i].field == field)
            break;
    if (i == viewport_count && viewport_count < MAX_VIEWPORTS) {
        viewports[viewport_count++] = (typeof(viewports[0])) { x, y, w, h, field };
        l_info("drawing area: x %u y %u w %u h %u (bottom at line %u of 480) - %s", x, y, w, h, y + h,
               field ? "field" : "not field");
    }
    return SO_CONTINUE(int, viewport_hook, x, y, w, h, arg);
}

void ff7_screen_patch(void) {
    base = so_mod.text_base;
    uintptr_t quad_tex = so_symbol(&so_mod, SYM_QUAD_TEX);
    uintptr_t border = so_symbol(&so_mod, SYM_BORDER);
    uintptr_t viewport = so_symbol(&so_mod, SYM_VIEWPORT);

    if (viewport && *(uint32_t *) (viewport & ~1) == VIEWPORT_PROLOGUE)
        viewport_hook = hook_addr(viewport, (uintptr_t) &viewport_hooked);
    else
        l_warn("ff7_screen: gfx_setviewport not found, drawing areas aren't logged");

    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_screen: %s found, field zoom off", OFF_SWITCH);
        return;
    }
    if (!quad_tex || !border || *(uint32_t *) (quad_tex & ~1) != QUAD_TEX_PROLOGUE
        || *(uint32_t *) (border & ~1) != BORDER_PROLOGUE
        || *(uintptr_t *) (base + CUR_VERTEX_GOT) != base + CUR_VERTEX) {
        l_warn("ff7_screen: unexpected .so layout, field zoom off");
        return;
    }
    cur_vertex = (uint8_t **) (base + CUR_VERTEX);
    quad_tex_hook = hook_addr(quad_tex, (uintptr_t) &quad_tex_hooked);
    border_hook = hook_addr(border, (uintptr_t) &border_hooked);
    zoom_enabled = 1;
    l_info("ff7_screen: field zoom installed (%.0f of %.0f lines fill the screen)", FIELD_LINES, FRAME_LINES);
}
