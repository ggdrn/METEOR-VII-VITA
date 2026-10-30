/*
 * Native 16:9 picture without stretching (docs/WIDESCREEN-16x9.md).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_WIDE_H
#define FF7_WIDE_H

#include <stdint.h>

/* Game pixels drawn beyond each side of the 640x480 frame while rendering wide */
#define WIDE_EXTRA 104

/** Hooks the game's ortho matrix and finds the field patch sites. After ff7_screen_patch(). */
void ff7_wide_patch(void);

/** The player's choice (port menu); applied on the game thread at its next drawing area. */
void ff7_wide_set(int on);
int ff7_wide_get(void);

/** 1 while the game draws its frame 848 pixels wide (x -104 .. 744). */
int ff7_wide_rendering(void);

/** Width of the picture a mode shows, in game pixels (640 when it stays 4:3). */
float ff7_wide_field_width(void);

/** World map: game pixels the projection's clip area grows by on each side (0: none). */
int ff7_wide_world_extra(void);

/** Field background tile window: background units it grows by on each side. */
int ff7_wide_tile_extra(void);

/** Game thread, once per field frame (field background renderer). */
void ff7_wide_field_frame(void);

/** Game thread, gl_queue: a draw's copied vertices (position, color, u/v arrays, stride 24). */
void ff7_wide_draw(uint32_t mode, int count, uint8_t * pos, uint32_t pos_stride, const uint8_t * color,
                   uint32_t color_stride, const uint8_t * uv, uint32_t uv_stride);

/** x offset of the 640-pixel frame inside the framebuffer, for glReadPixels. */
int ff7_wide_read_offset(void);

void ff7_wide_report(double seconds);

#endif // FF7_WIDE_H
