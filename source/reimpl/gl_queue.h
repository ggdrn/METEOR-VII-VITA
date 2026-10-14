/*
 * Render thread: the game thread's GL calls run on another core
 * (see gl_queue.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_GL_QUEUE_H
#define SOLOADER_GL_QUEUE_H

#include <vitaGL.h>

/** Starts the render thread. Call after gl_init(). */
void gl_queue_init(void);

/** Called from the game thread before it runs: its GL calls get queued. */
void gl_queue_set_game_thread(void);

/** Waits until every queued GL call has run. Call before using GL directly
 *  (outside the gq_* entry points) from the game thread or another thread. */
void gl_queue_finish(void);

/** Locks out the render thread around direct GL use from another thread
 *  (e.g. vglSwapBuffers on the main thread). Waits for the queue first. */
void gl_queue_lock(void);
void gl_queue_unlock(void);

/** Game thread time (us) spent in the GL entry points since the last call. */
unsigned long long gl_queue_take_game_us(void);

/** 1 if GL calls from the game thread are being queued. */
int gl_queue_active(void);

/** Logs render thread statistics over `seconds`, then resets them. */
void gl_queue_report(double seconds);

/* Entry points for the GL functions libjni_ff7.so imports (dynlib.c) */
void gq_glActiveTexture(GLenum texture);
void gq_glAttachShader(GLuint program, GLuint shader);
void gq_glBindAttribLocation(GLuint program, GLuint index, const GLchar * name);
void gq_glBindFramebuffer(GLenum target, GLuint framebuffer);
void gq_glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void gq_glBindTexture(GLenum target, GLuint texture);
void gq_glBlendFunc(GLenum sfactor, GLenum dfactor);
GLenum gq_glCheckFramebufferStatus(GLenum target);
void gq_glClear(GLbitfield mask);
void gq_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gq_glClearDepthf(GLfloat depth);
void gq_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void gq_glCompileShader(GLuint shader);
GLuint gq_glCreateProgram(void);
GLuint gq_glCreateShader(GLenum type);
void gq_glCullFace(GLenum mode);
void gq_glDeleteProgram(GLuint program);
void gq_glDeleteShader(GLuint shader);
void gq_glDeleteTextures(GLsizei n, const GLuint * textures);
void gq_glDepthFunc(GLenum func);
void gq_glDepthMask(GLboolean flag);
void gq_glDepthRangef(GLfloat n, GLfloat f);
void gq_glDisable(GLenum cap);
void gq_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void gq_glEnable(GLenum cap);
void gq_glEnableVertexAttribArray(GLuint index);
void gq_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer);
void gq_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void gq_glFrontFace(GLenum mode);
void gq_glGenFramebuffers(GLsizei n, GLuint * ids);
void gq_glGenRenderbuffers(GLsizei n, GLuint * ids);
void gq_glGenTextures(GLsizei n, GLuint * ids);
GLenum gq_glGetError(void);
void gq_glGetIntegerv(GLenum pname, GLint * data);
void gq_glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei * length, GLchar * log);
void gq_glGetProgramiv(GLuint program, GLenum pname, GLint * params);
void gq_glGetShaderInfoLog(GLuint shader, GLsizei size, GLsizei * length, GLchar * log);
void gq_glGetShaderiv(GLuint shader, GLenum pname, GLint * params);
const GLubyte * gq_glGetString(GLenum name);
GLint gq_glGetUniformLocation(GLuint program, const GLchar * name);
void gq_glLineWidth(GLfloat width);
void gq_glLinkProgram(GLuint program);
void gq_glPixelStorei(GLenum pname, GLint param);
void gq_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void * pixels);
void gq_glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h);
void gq_glShaderSource(GLuint shader, GLsizei count, const GLchar ** string, const GLint * length);
void gq_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h,
                     GLint border, GLenum format, GLenum type, const void * pixels);
void gq_glTexParameteri(GLenum target, GLenum pname, GLint param);
void gq_glUniform1i(GLint location, GLint v0);
void gq_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
void gq_glUseProgram(GLuint program);
void gq_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                              const void * pointer);
void gq_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);

#endif // SOLOADER_GL_QUEUE_H
