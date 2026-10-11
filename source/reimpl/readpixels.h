/*
 * glReadPixels cache for row-by-row screen captures (see readpixels.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_READPIXELS_H
#define SOLOADER_READPIXELS_H

#include <vitaGL.h>

void glReadPixels_cached(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels);

/** Call whenever the framebuffer contents may change (draw, clear, bind, swap). */
void readpixels_invalidate(void);

#endif // SOLOADER_READPIXELS_H
