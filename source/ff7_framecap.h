/*
 * 30 fps cadence for the 3D minigames: snowboard and roller coaster (60 Hz
 * logic) and the submarine's own limiter (ff7_framecap.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_FRAMECAP_H
#define FF7_FRAMECAP_H

void ff7_framecap_patch(void);
void ff7_framecap_report(double seconds);
/** Port menu A/B test. */
void ff7_framecap_set(int on);

#endif // FF7_FRAMECAP_H
