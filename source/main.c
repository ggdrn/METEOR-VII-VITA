/*
 * FINAL FANTASY VII (Android v1.0.29) loader for PS Vita.
 *
 * Drives libjni_ff7.so the same way MainActivity/RendererWrapper do on
 * Android (see PLANO.md for the decompiled call sequence).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"

#include <stdbool.h>
#include <stdio.h>

#include <psp2/apputil.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

// jp.co.d4e.materialg.GLESJniWrapper natives (static: JNIEnv*, jclass, ...)
static void (* ff7_setLang)(JNIEnv *, jclass, jint);
static void (* ff7_setDataPath)(JNIEnv *, jclass, jstring);
static void (* ff7_setAssetManager)(JNIEnv *, jclass, jobject);
static void (* ff7_onResume)(JNIEnv *, jclass);
static void (* ff7_onPause)(JNIEnv *, jclass);
static void (* ff7_onSurfaceChanged)(JNIEnv *, jclass, jint, jint);
static void (* ff7_onSurfaceCreated)(JNIEnv *, jclass, jboolean, jboolean);
static void (* ff7_onDrawFrame)(JNIEnv *, jclass);
static void (* ff7_onKey)(JNIEnv *, jclass, jint, jboolean);
static jint (* ff7_onKeyBack)(JNIEnv *, jclass);
static void (* ff7_onTouchBegan)(JNIEnv *, jclass, jint, jfloat, jfloat);
static void (* ff7_onTouchMoved)(JNIEnv *, jclass, jint, jfloat, jfloat);
static void (* ff7_onTouchEnded)(JNIEnv *, jclass, jint, jfloat, jfloat);

#define FF7_CLASS ((jclass) 0x42424242)

static void * ff7_symbol(const char * name) {
    char full[128];
    snprintf(full, sizeof(full), "Java_jp_co_d4e_materialg_GLESJniWrapper_%s", name);
    void * sym = (void *) so_symbol(&so_mod, full);
    if (!sym) l_error("Missing native: %s", full);
    return sym;
}

// MainActivity.onCreate(): en=0, fr=1, de=2, es=3, ja=4 (anything else: en)
static int ff7_lang_from_system() {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_FRENCH:     return 1;
        case SCE_SYSTEM_PARAM_LANG_GERMAN:     return 2;
        case SCE_SYSTEM_PARAM_LANG_SPANISH:    return 3;
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:   return 4;
        default:                               return 0;
    }
}

int main() {
    soloader_init_all();

    ff7_setLang          = ff7_symbol("setLang");
    ff7_setDataPath      = ff7_symbol("setDataPath");
    ff7_setAssetManager  = ff7_symbol("setAssetManager");
    ff7_onResume         = ff7_symbol("onResume");
    ff7_onPause          = ff7_symbol("onPause");
    ff7_onSurfaceChanged = ff7_symbol("onSurfaceChanged");
    ff7_onSurfaceCreated = ff7_symbol("onSurfaceCreated");
    ff7_onDrawFrame      = ff7_symbol("onDrawFrame");
    ff7_onKey            = ff7_symbol("onKey");
    ff7_onKeyBack        = ff7_symbol("onKeyBack");
    ff7_onTouchBegan     = ff7_symbol("onTouchBegan");
    ff7_onTouchMoved     = ff7_symbol("onTouchMoved");
    ff7_onTouchEnded     = ff7_symbol("onTouchEnded");

    int (* JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    l_info("JNI_OnLoad: %i", JNI_OnLoad(&jvm));

    // MainActivity.onCreate()
    int lang = ff7_lang_from_system();
    l_info("setLang(%i)", lang);
    ff7_setLang(&jni, FF7_CLASS, lang);

    l_info("setDataPath(\"%s\")", SAVE_PATH);
    ff7_setDataPath(&jni, FF7_CLASS, jni->NewStringUTF(&jni, SAVE_PATH));

    // MainActivity.onResume()
    l_info("setAssetManager / onResume");
    ff7_setAssetManager(&jni, FF7_CLASS, (jobject) 0x41414141);
    ff7_onResume(&jni, FF7_CLASS);

    // GLSurfaceView: RendererWrapper.onSurfaceChanged() -> onSurfaceCreated()
    gl_init();
    l_info("onSurfaceChanged(960, 544)");
    ff7_onSurfaceChanged(&jni, FF7_CLASS, 960, 544);
    l_info("onSurfaceCreated(false, false)");
    ff7_onSurfaceCreated(&jni, FF7_CLASS, JNI_FALSE, JNI_FALSE);

    l_success("Entering main loop.");
    unsigned int frame = 0;
    while (1) {
        controls_poll();

        // RendererWrapper.onDrawFrame() skips the first two frames
        if (++frame >= 3) {
            if (frame == 3) l_info("First onDrawFrame()");
            ff7_onDrawFrame(&jni, FF7_CLASS);
            if (frame == 3) l_info("First onDrawFrame() returned");
        }

        gl_swap();
    }

    sceKernelExitDeleteThread(0);
}

/*
 * Input. Button ids follow MainActivity.getKeyId():
 * 0 up, 1 left, 2 down, 3 right, 4 A, 5 B, 6 X, 7 Y,
 * 8 L1, 9 R1, 10 start, 11 select, 12 L2, 13 R2
 */

static int ff7_button_from_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_UP:       return 0;
        case AKEYCODE_DPAD_LEFT:     return 1;
        case AKEYCODE_DPAD_DOWN:     return 2;
        case AKEYCODE_DPAD_RIGHT:    return 3;
        case AKEYCODE_BUTTON_A:      return 4;
        case AKEYCODE_BUTTON_B:      return 5;
        case AKEYCODE_BUTTON_X:      return 6;
        case AKEYCODE_BUTTON_Y:      return 7;
        case AKEYCODE_BUTTON_L1:     return 8;
        case AKEYCODE_BUTTON_R1:     return 9;
        case AKEYCODE_BUTTON_START:  return 10;
        case AKEYCODE_BUTTON_SELECT: return 11;
        case AKEYCODE_BUTTON_L2:     return 12;
        case AKEYCODE_BUTTON_R2:     return 13;
        default:                     return -1;
    }
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    int button = ff7_button_from_keycode(keycode);
    if (button < 0 || action == CONTROLS_ACTION_MOVE) return;
    ff7_onKey(&jni, FF7_CLASS, button, action == CONTROLS_ACTION_DOWN);
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // MainActivity.MakeIdForFF7(): pointer id + 1
    switch (action) {
        case CONTROLS_ACTION_DOWN: ff7_onTouchBegan(&jni, FF7_CLASS, id + 1, x, y); break;
        case CONTROLS_ACTION_MOVE: ff7_onTouchMoved(&jni, FF7_CLASS, id + 1, x, y); break;
        case CONTROLS_ACTION_UP:   ff7_onTouchEnded(&jni, FF7_CLASS, id + 1, x, y); break;
    }
}

// Left stick doubles as the d-pad
#define STICK_THRESHOLD 0.5f

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    static bool held[4]; // up, left, down, right
    if (which != CONTROLS_STICK_LEFT) return;

    const bool now[4] = { y < -STICK_THRESHOLD, x < -STICK_THRESHOLD,
                          y > STICK_THRESHOLD,  x > STICK_THRESHOLD };
    for (int i = 0; i < 4; i++) {
        if (now[i] != held[i]) {
            held[i] = now[i];
            ff7_onKey(&jni, FF7_CLASS, i, now[i]);
        }
    }
}
