/*
 * PlayStation button glyphs on the game's button atlas (see ff7_buttons.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_BUTTONS_H
#define FF7_BUTTONS_H

#include <stdint.h>

/** Reads psbuttons_off.txt and hooks the world map init. Call from so_patch(). */
void ff7_buttons_patch(void);

/** Redraws A/B/X/Y as Cross/Circle/Square/Triangle. Returns glyphs drawn. */
int ff7_buttons_patch_atlas(const char * path, uint32_t * argb, int w, int h);

/** Before convert_image_data(th): draws the symbols into the menu font. */
void ff7_buttons_font(const void * th);

/** After the LZSS decompressor wrote to x86 address `dst`: field text labels. */
void ff7_buttons_lzss(uint32_t dst);

#endif // FF7_BUTTONS_H
