/*
 * See ff7_math.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_MATH_H
#define FF7_MATH_H

/** Installs the native replacement. Call after ff7_anim_patch(). */
void ff7_math_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_math_report(double seconds);

#endif // FF7_MATH_H
