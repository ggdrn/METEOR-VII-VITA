/*
 * Port menu (Start + L + R): cheats, gameplay options, minigame shortcuts and profiler control
 * (see port_menu.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef PORT_MENU_H
#define PORT_MENU_H

#include <stdint.h>

/** Before setLang: the dialog language chosen in the menu (0 en, 1 fr, 2 de,
 *  3 es, 4 ja), or -1 for the Vita's system language. */
int port_menu_language(void);

/** Main thread, once after the .so is loaded. */
void port_menu_init(void);

/** Main thread, every pad poll: returns the buttons to pass on to the game
 *  (none while the menu is open; the Start of the combo is held back). */
uint32_t port_menu_input(uint32_t buttons);

/** 1 while the menu is open (the game gets no input). */
int port_menu_is_open(void);

/** Main thread, with GL locked, right before the buffer swap. */
void port_menu_draw(void);

/** Game thread, between frames (frame limiter sleep): applies a pending
 *  minigame launch and follows the minigame for the profiler. */
void port_menu_game_tick(void);

/** Game thread, between frames: applies the A/B test switches of the menu. */
void port_menu_apply_ab(void);

#endif // PORT_MENU_H
