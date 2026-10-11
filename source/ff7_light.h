/*
 * Native per-vertex lighting for field models (see ff7_light.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_LIGHT_H
#define FF7_LIGHT_H

/** Hooks func_0x0068dd1e. Call after ff7_anim_patch(). */
void ff7_light_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_light_report(double seconds);

#endif // FF7_LIGHT_H
