/*
 * Native field background tile lists (ff7_fieldbg.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_FIELDBG_H
#define FF7_FIELDBG_H

void ff7_fieldbg_patch(void);
void ff7_fieldbg_report(double seconds);

/** 1 if the native tile lists run (16:9 widens their window). */
int ff7_fieldbg_installed(void);

#endif // FF7_FIELDBG_H
