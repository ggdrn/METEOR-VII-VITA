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

/**
 * The game buttons (bit = ff7_onKey id) held by the on-screen buttons kept
 * in the current layout. Main thread (where the touches are handled).
 */
unsigned ff7_ui_touch_buttons(void);

/** Native Android flag: 1 ON, 0 OFF, -1 if the .so layout is unsupported. */
int ff7_ui_encounters_get(void);
void ff7_ui_encounters_set(int on);

/** Visibility only; removing the touch target does not change encounters. */
int ff7_ui_encounters_button_get(void);
void ff7_ui_encounters_button_set(int show);

#endif // FF7_UI_H
