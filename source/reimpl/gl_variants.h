/*
 * Specialized fragment programs for the game's branching shader.
 * See gl_variants.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_GL_VARIANTS_H
#define SOLOADER_GL_VARIANTS_H

#include <vitaGL.h>

/** Called with every (rewritten) shader source the game sets. */
void gl_variants_shader_source(GLuint shader, const char * src);

void glAttachShader_variants(GLuint prog, GLuint shader);
GLint glGetUniformLocation_variants(GLuint prog, const GLchar * name);
void glUseProgram_variants(GLuint prog);
void glUniform1i_variants(GLint loc, GLint value);
void glUniformMatrix4fv_variants(GLint loc, GLsizei count, GLboolean transpose, const GLfloat * value);

/** Binds the program for the next draw. */
int gl_variants_before_draw(void);

/** Logs draws per variant since the last report. */
void gl_variants_report(double seconds);

#endif // SOLOADER_GL_VARIANTS_H
