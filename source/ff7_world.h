/*
 * Native world map vertex projection (see ff7_world.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_WORLD_H
#define FF7_WORLD_H

/** Hooks func_0x0075f263. Call after ff7_anim_patch(), before ff7_memmap_patch(). */
void ff7_world_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_world_report(double seconds);

#endif // FF7_WORLD_H
