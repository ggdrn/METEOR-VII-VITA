/*
 * PlayStation button glyphs on the game's button atlas (see ff7_buttons.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_BUTTONS_H
#define FF7_BUTTONS_H

#include <stdint.h>

/** Redraws A/B/X/Y as Cross/Circle/Square/Triangle. Returns glyphs drawn. */
int ff7_buttons_patch_atlas(const char * path, uint32_t * argb, int w, int h);

#endif // FF7_BUTTONS_H
