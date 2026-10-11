/*
 * Touch UI adjustments for libjni_ff7.so (see ff7_ui.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef FF7_UI_H
#define FF7_UI_H

/** Hides the virtual gamepad and logs the first touches. Call after relocation. */
void ff7_ui_patch(void);

#endif // FF7_UI_H
