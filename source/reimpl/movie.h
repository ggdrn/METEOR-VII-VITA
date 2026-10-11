/*
 * MyDecoder (FMV player) reimplementation.
 *
 * The FF7 engine syncs field scripts to the movie clock (e.g. the opening
 * leads straight into the Bombing Mission), so a movie that "fails" freezes
 * the game. Until real VP8 decoding lands (Fase 5), movies play "silently":
 * timing comes from the WebM header and the screen stays black.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_MOVIE_H
#define SOLOADER_MOVIE_H

#include <stdbool.h>

/** MyDecoder.START: `fname` as passed by the game ("/ff7_1.02/.../x.avi"). */
bool movie_start(const char * fname);

/** MyDecoder.FRAME: waits for the next frame; false once the movie is over. */
bool movie_frame(void);

/** MyDecoder.RESET */
void movie_stop(void);

int movie_get_total_ms(void);
int movie_get_position_ms(void);
void movie_set_position_ms(int pos);

/** MainActivity onPause/onResume */
void movie_pause(bool pause);

#endif // SOLOADER_MOVIE_H
