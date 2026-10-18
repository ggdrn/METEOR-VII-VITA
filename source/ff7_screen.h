/*
 * Field mode zoom of the final picture (see ff7_screen.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SCREEN_H
#define FF7_SCREEN_H

/** Hooks VIDEO_update's quads and gfx_setviewport. Call after ff7_bg_patch()
 *  and before ff7_memmap_patch(). */
void ff7_screen_patch(void);

#endif // FF7_SCREEN_H
