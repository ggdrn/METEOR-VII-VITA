/*
 * Lightweight per-second counters for the GL calls that matter most for
 * performance on the Vita, reported with the heartbeat in logs.txt.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_GLSTATS_H
#define SOLOADER_GLSTATS_H

#include <vitaGL.h>

void glDrawArrays_stat(GLenum mode, GLint first, GLsizei count);
void glTexImage2D_stat(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                       GLint border, GLenum format, GLenum type, const void * pixels);
void glReadPixels_stat(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels);
void glFlush_stat(void);
void glBindFramebuffer_stat(GLenum target, GLuint framebuffer);
void glClear_stat(GLbitfield mask);

/* Wait/sleep wrappers: time is accounted only for the game (GL) thread */
int usleep_stat(unsigned int usec);
int pthread_mutex_lock_stat(void * mutex);
int pthread_cond_wait_stat(void * cond, void * mutex);
int sem_wait_stat(int * sem);

/** Logs the counters accumulated since the last call over `seconds`, then resets them. */
void glstats_report(double seconds);

#endif // SOLOADER_GLSTATS_H
