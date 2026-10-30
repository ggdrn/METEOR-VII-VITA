/*
 * Field mode zoom of the final picture for libjni_ff7.so.
 *
 * The game draws each frame into a 640x480 offscreen texture. VIDEO_update
 * (on the main thread) then draws it on the screen as one quad as tall as the
 * screen (725x544 on the Vita, centered), followed by the side borders and the
 * touch UI. In field mode the game leaves the last 16 of the 480 lines empty,
 * which showed as a black strip at the bottom of the screen.
 *
 * The field's drawing area is 640x448 (gfx_setviewport, logged below): the
 * last 32 of the 480 lines are empty. v0.2.02 assumed 464 lines, which still
 * left an 18 px strip. Now the zoom follows the drawing area the game sets
 * in field mode (448 lines unless a field asks for another full-width area).
 *
 * While the field background renderer runs (ff7_bg_last_us), the frame's quad
 * is scaled by 480/448 around the top center: the field fills the screen
 * height (777x544 on the Vita), the empty lines fall below the screen, and
 * the picture keeps its aspect, with nothing cut on the sides. Stretching to
 * the whole 960x544 would distort it by 23% and filling the width without
 * distortion would cut 128 of the 544 lines, so neither is the default:
 * DATA_PATH "field_stretch_on.txt" stretches the field to the full width for
 * those who prefer it.
 *
 * Battles: the 3D scene and the command menu end at line 448, but the
 * submenus (magic, items...) open lower, down to about line 465. While
 * ff7.exe's game mode (word at VA GAME_MODE_VA, written by the module switch)
 * is MODE_BATTLE, a milder zoom keeps BATTLE_LINES lines: the black strip
 * under the command menu shrinks and the submenus still fit (v0.5.02 used
 * 448 there and cut their last row). The battle camera
 * is the game's own (made for 448 lines): showing more of the scene below
 * would mean a new projection and moving every battle window, so the zoom
 * enlarges the picture instead. The side borders are shortened to match. The UI
 * ("Encounters ON", "A") is drawn afterwards in its usual place, on top.
 * A 640x448 movie uses the same final quad. The MyDecoder state is the
 * authoritative signal that an FMV is playing, so it is zoomed like a field
 * without relying on a game mode shared by menus and transitions. A movie at
 * any other height keeps the original presentation. Menus and the world map
 * are drawn as before.
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
 * 16:9 (ff7_wide.c): the game draws 848 pixels across (x -104 .. 744) into
 * the same framebuffer. The texture coordinates of the frame quad then pick
 * the part shown: the central 640 pixels, or in a field, battle or the world
 * map as many as fill the screen at that zoom (791, 833 and 847). Field
 * dialogue shares the scene's uniform pixel scale: a narrow background
 * never stretches the final composite. The world map keeps its 480 lines.
 * The stretch switch applies only to 4:3 pictures.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "ff7_screen.h"
#include "ff7_unhook.h"
#include "ff7_bg.h"
#include "ff7_wide.h"
#include "reimpl/movie.h"

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
#define STRETCH_SWITCH   DATA_PATH "field_stretch_on.txt"
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
#define FIELD_LINES      448        // field drawing area: x 0 y 0 w 640 h 448
#define MIN_FIELD_LINES  400        // full-width areas shorter than this aren't the field's
#define BATTLE_LINES     472        // battle submenus reach ~line 465 (the 3D area ends at 448)
#define FIELD_TIMEOUT_US 250000     // field mode ends this long after the last background frame
#define VERTEX_SIZE      0x18
#define QUAD_VERTICES    6
#define MAX_VIEWPORTS    32
#define GAME_MODE_VA     0xcbf9dc   // uint16_t: 1 field, 2 battle, 3 world map...
#define MODE_BATTLE      2
#define MODE_WORLD       3
#define MODE_FIELD       1
#define MODE_SWIRL       23

static so_hook quad_tex_hook, border_hook, viewport_hook;
static uint8_t ** cur_vertex;
static uintptr_t base;
static int zoom_enabled, zoomed, stretch, quad_installed;

int ff7_screen_quad_installed(void) { return quad_installed; }
int ff7_screen_zoom_enabled(void) { return zoom_enabled; }
// Lines the picture fills, per mode: set by the game thread, read by the main thread
static volatile uint32_t field_lines = FIELD_LINES;
static uint32_t logged_lines, logged_shown;
static volatile uint16_t * game_mode;
static int logged_mode = -1;

static const uint32_t * memmap; // ff7.exe's 4 KB page table (see ff7_memmap.c)

// ff7.exe's memory is mapped once the game runs (a mapped page never moves);
// until then, no mode. Read from MEMMAP directly: mk_address would hand out
// its dummy page for an unmapped address. A save loaded straight into the
// world map never runs the field renderer, so that can't be the signal.
static int current_mode(void) {
    if (!game_mode) {
        uint32_t page = memmap ? memmap[GAME_MODE_VA >> 12] : 0;
        if (!page) return 0;
        game_mode = (volatile uint16_t *) (page + (GAME_MODE_VA & 0xfff));
    }
    return *game_mode;
}

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

// Choose the source rectangle independently from its placement on screen.
static void frame_quad_adjust(int mode) {
    if (mode != logged_mode) {
        logged_mode = mode;
        l_info("game mode: %d", mode);
    }
    int wide = ff7_wide_rendering();
    int playing = movie_is_playing();
    int field = zoom_enabled && mode == MODE_FIELD && !playing && in_field();
    int movie = zoom_enabled && playing && movie_video_height() == FIELD_LINES;
    int battle = zoom_enabled && !field && !movie && mode == MODE_BATTLE;
    int world = wide && !field && !movie && !battle && mode == MODE_WORLD;
    // The swirl module still draws a 640-pixel captured image. Present that
    // whole image across the display; do not sample nonexistent side pixels.
    int swirl = ff7_wide_get() && !playing && mode == MODE_SWIRL;
    int on = field ? 1 : movie ? 2 : battle ? 3 : world ? 4 : swirl ? 5 : 0;
    if (on != zoomed) {
        zoomed = on;
        l_info("field zoom: %s", on == 1 ? "on (field)" : on == 2 ? "on (movie)" : on == 3 ? "on (battle)"
                                 : on == 4 ? "on (world map, 16:9)"
                                 : on == 5 ? "on (battle swirl, stretched 16:9)" : "off");
    }
    if (!on && !wide) {
        zoom_left = zoom_right = 0;
        return;
    }

    // The quad VIDEO_doQuad just wrote: x0..x1 by 0..screen height (y down)
    uint32_t lines = field ? field_lines : movie ? FIELD_LINES : battle ? BATTLE_LINES : (uint32_t) FRAME_LINES;
    uint8_t * end = *cur_vertex;
    float x0 = vertex(end, 0)[0], x1 = vertex(end, 1)[0];
    const float k = FRAME_LINES / (float) lines;
    zoom_center = (x0 + x1) * 0.5f;
    // Game pixels shown across: 640, or more in 16:9 (as many as fill the screen at this zoom)
    float shown = 640.0f;
    float display = shown;
    if (swirl && x1 > x0) display = 640.0f * zoom_center / ((zoom_center - x0) * k);
    if (wide && (field || battle || world) && x1 > x0) {
        float fit = 640.0f * zoom_center / ((zoom_center - x0) * k);
        shown = fit;
        if (shown > fit) shown = fit;
        if (shown > 640.0f + 2 * WIDE_EXTRA) shown = 640.0f + 2 * WIDE_EXTRA;
        if (shown < 640.0f) shown = 640.0f;
        // One uniform pixel scale for scenery and dialogue. Missing artwork
        // remains clear; never stretch the composited dialogue to fill it.
        display = shown;
    }
    if ((uint32_t) shown != logged_shown || lines != logged_lines) {
        logged_shown = (uint32_t) shown;
        logged_lines = lines;
        l_info("field zoom: %u of 480 lines, %u game pixels across fill the screen%s", lines, logged_shown,
               display > shown + 0.01f || (stretch && shown == 640.0f && on)
                   ? ", stretched to the full width" : "");
    }
    // The quad is centered on the screen: the screen is 2 * center wide
    float kx = k * display / 640.0f;
    if (stretch && on && shown == 640.0f && x1 > x0) kx = zoom_center / (zoom_center - x0); // edges at 0 and the screen width
    zoom_left = zoom_center - (zoom_center - x0) * kx;
    zoom_right = zoom_center + (x1 - zoom_center) * kx;
    // 16:9: the framebuffer holds x -104 .. 744; texture coordinates of the part shown
    float u0 = vertex(end, 0)[4], du = (vertex(end, 1)[4] - u0) / 640.0f;
    float ul = u0 + du * (WIDE_EXTRA + 320.0f - shown * 0.5f), ur = u0 + du * (WIDE_EXTRA + 320.0f + shown * 0.5f);
    for (int i = 0; i < QUAD_VERTICES; i++) {
        float * v = vertex(end, i);
        if (wide) v[4] = v[0] < zoom_center ? ul : ur; // u, before x moves
        v[0] = zoom_center + (v[0] - zoom_center) * kx;
        if (on) v[1] *= k; // top stays at 0, the empty lines go below the screen
    }
}

// Float arguments stay as raw bits across the softfp hook.
static int quad_tex_hooked(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    int result = SO_CONTINUE_HELD(int, quad_tex_hook, x, y, w, h);
    if ((uintptr_t) __builtin_return_address(0) == base + RET_FRAME_QUAD)
        frame_quad_adjust(current_mode());
    return result;
}

static int border_hooked(int r0, int g0, int b0, int r1, int g1, int b1) {
    int r = SO_CONTINUE_HELD(int, border_hook, r0, g0, b0, r1, g1, b1);
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
    int field = current_mode() == MODE_FIELD && !movie_is_playing() && in_field();
    // The field's own full-width area (smaller ones are windows and effects;
    // a full 480-line area right at a mode change must not stick)
    if (field && x == 0 && y == 0 && w == 640 && h >= MIN_FIELD_LINES && h < 480)
        field_lines = h;
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
    return SO_CONTINUE_HELD(int, viewport_hook, x, y, w, h, arg);
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

    if (!quad_tex || !border || *(uint32_t *) (quad_tex & ~1) != QUAD_TEX_PROLOGUE
        || *(uint32_t *) (border & ~1) != BORDER_PROLOGUE
        || *(uintptr_t *) (base + CUR_VERTEX_GOT) != base + CUR_VERTEX) {
        l_warn("ff7_screen: unexpected .so layout, field zoom off");
        return;
    }
    cur_vertex = (uint8_t **) (base + CUR_VERTEX);
    memmap = (const uint32_t *) so_symbol(&so_mod, "MEMMAP");
    quad_tex_hook = hook_addr(quad_tex, (uintptr_t) &quad_tex_hooked);
    border_hook = hook_addr(border, (uintptr_t) &border_hooked);
    quad_installed = 1;
    // Installed even with the zoom off: a 16:9 frame (ff7_wide.c) always needs its part chosen
    if (file_exists(OFF_SWITCH)) {
        l_info("ff7_screen: %s found, field zoom off", OFF_SWITCH);
        return;
    }
    zoom_enabled = 1;
    stretch = file_exists(STRETCH_SWITCH);
    l_info("ff7_screen: field zoom installed (%u of 480 lines fill the screen%s)", FIELD_LINES,
           stretch ? ", stretched to the full width by " STRETCH_SWITCH : "");
}
