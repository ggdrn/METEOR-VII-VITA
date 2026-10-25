/*
 * Native functions of the roller coaster minigame (see ff7_coaster.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_COASTER_H
#define FF7_COASTER_H

void ff7_coaster_patch(void);
void ff7_coaster_report(double seconds);
/** Port menu A/B test, on the game thread between frames. */
void ff7_coaster_set(int on);

#endif // FF7_COASTER_H
