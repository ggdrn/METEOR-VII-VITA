/*
 * GL_OES_EGL_image_external on top of plain 2D textures.
 *
 * On Android the game binds its movie texture as GL_TEXTURE_EXTERNAL_OES
 * (filled by a SurfaceTexture). vitaGL has no external textures: the movie
 * player uploads frames as regular 2D (YUV) textures and the shaders are
 * rewritten to use sampler2D (utils/glutil.c), so the target is mapped here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <vitaGL.h>

#include "utils/logger.h"

#define GL_TEXTURE_EXTERNAL_OES 0x8D65

static inline GLenum map_target(GLenum target) {
    if (target == GL_TEXTURE_EXTERNAL_OES) {
        static int logged;
        if (!logged++) l_info("GL_TEXTURE_EXTERNAL_OES mapped to GL_TEXTURE_2D");
        return GL_TEXTURE_2D;
    }
    return target;
}

void glBindTexture_oes(GLenum target, GLuint texture) {
    glBindTexture(map_target(target), texture);
}

void glTexParameteri_oes(GLenum target, GLenum pname, GLint param) {
    glTexParameteri(map_target(target), pname, param);
}
