/*
 * Native 16-bit screen copy for the battle swirl (see ff7_swirl.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_SWIRL_H
#define FF7_SWIRL_H

/** Hooks func_0x00673d26. Call after ff7_anim_patch(). */
void ff7_swirl_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_swirl_report(double seconds);

#endif // FF7_SWIRL_H
