/*
 * Native highway (motorbike minigame) vertex color (see ff7_highway.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_HIGHWAY_H
#define FF7_HIGHWAY_H

/** Hooks func_0x00651a6d. Call before ff7_memmap_patch(). */
void ff7_highway_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_highway_report(double seconds);

#endif // FF7_HIGHWAY_H
