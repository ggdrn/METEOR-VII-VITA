/*
 * See ff7_bg.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_BG_H
#define FF7_BG_H

#include <stdint.h>

/** Process time (us) of the last field background frame: the game is in
 *  field mode while this keeps advancing (see ff7_screen.c). */
extern volatile uint64_t ff7_bg_last_us;

/** Installs the native replacement. Call after ff7_anim_patch(). */
void ff7_bg_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_bg_report(double seconds);

#endif // FF7_BG_H
