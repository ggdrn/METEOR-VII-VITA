/*
 * Native triangle renderer C_006A8002 (see ff7_tri.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_TRI_H
#define FF7_TRI_H

/** Hooks C_006A8002. Call after relocation. */
void ff7_tri_patch(void);

/** Logs statistics accumulated over `seconds`, then resets them. */
void ff7_tri_report(double seconds);

#endif // FF7_TRI_H
