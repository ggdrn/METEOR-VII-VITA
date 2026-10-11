/*
 * EGL entry points used by the game that vitaGL doesn't implement.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/egl.h"
#include "utils/logger.h"

EGLDisplay eglGetCurrentDisplay(void) {
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface,
                            EGLint attribute, EGLint value) {
    // Called every frame by the game: log only the first time
    static int logged;
    if (!logged++) l_info("eglSurfaceAttrib(0x%x, 0x%x): ignored", attribute, value);
    return EGL_TRUE;
}
