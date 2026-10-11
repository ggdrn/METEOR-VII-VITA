/*
 * Java side of FINAL FANTASY VII (jp.co.d4e.materialg.*), reimplemented for
 * FalsoJNI. Signatures come from the decompiled classes.dex of v1.0.29.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stb/stb_image.h>

#include "reimpl/asset_manager.h"
#include "reimpl/obb_fd.h"
#include "utils/logger.h"
#include "utils/zip.h"

extern zip_archive * obb_zip;

static const char * jstr_get(jstring s) {
    return s ? jni->GetStringUTFChars(&jni, s, NULL) : NULL;
}

static void jstr_release(jstring s, const char * c) {
    if (s && c) jni->ReleaseStringUTFChars(&jni, s, (char *) c);
}

/*
 * jp.co.d4e.materialg.ExpansionFile
 *
 * On Android the game gets a descriptor of the .obb plus the offset/length of
 * an entry, then seeks and reads by itself. The OBB is an uncompressed zip, so
 * the original file is served as-is.
 */

// ExpansionFile keeps one descriptor per path until CLOSE_FILE_DESCRIPTOR
#define MAX_OBB_PATHS 32

static struct {
    char path[256];
    int fd;
} obb_fds[MAX_OBB_PATHS];

static zip_entry * obb_find(const char * path) {
    if (!obb_zip || !path) return NULL;

    // The exact form the game uses is logged; try the likely variants
    while (path[0] == '/') path++;
    zip_entry * e = zip_find(obb_zip, path);
    if (!e) {
        char buf[512];
        snprintf(buf, sizeof(buf), "assets/%s", path);
        e = zip_find(obb_zip, buf);
    }
    return e;
}

static void obb_close_fd(const char * path) {
    for (int i = 0; i < MAX_OBB_PATHS; i++) {
        if (obb_fds[i].path[0] && strcmp(obb_fds[i].path, path) == 0) {
            obb_fd_close(obb_fds[i].fd);
            obb_fds[i].path[0] = '\0';
        }
    }
}

jint ExpansionFile_OPEN_FILE_DESCRIPTOR(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    int ret = -1;

    if (!obb_find(path)) {
        l_warn("ExpansionFile.OPEN_FILE_DESCRIPTOR(\"%s\"): not in OBB", path);
        goto out;
    }

    obb_close_fd(path);
    for (int i = 0; i < MAX_OBB_PATHS; i++) {
        if (!obb_fds[i].path[0]) {
            const char * base = strrchr(path, '/');
            ret = obb_fd_open(base ? base + 1 : path);
            if (ret >= 0) {
                strncpy(obb_fds[i].path, path, sizeof(obb_fds[i].path) - 1);
                obb_fds[i].fd = ret;
            }
            break;
        }
    }
    l_info("ExpansionFile.OPEN_FILE_DESCRIPTOR(\"%s\"): %i", path, ret);

out:
    jstr_release(_path, path);
    return ret;
}

void ExpansionFile_CLOSE_FILE_DESCRIPTOR(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    l_info("ExpansionFile.CLOSE_FILE_DESCRIPTOR(\"%s\")", path);
    if (path) obb_close_fd(path);
    jstr_release(_path, path);
}

jlong ExpansionFile_GET_START_OFFSET(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    zip_entry * e = obb_find(path);
    jlong ret = e ? zip_entry_data_offset(obb_zip, e) : -1;
    l_info("ExpansionFile.GET_START_OFFSET(\"%s\"): %lld", path, ret);
    jstr_release(_path, path);
    return ret;
}

jlong ExpansionFile_GET_LENGTH(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    zip_entry * e = obb_find(path);
    jlong ret = e ? (jlong) e->uncomp_size : -1;
    l_info("ExpansionFile.GET_LENGTH(\"%s\"): %lld", path, ret);
    jstr_release(_path, path);
    return ret;
}

/*
 * jp.co.d4e.materialg.MainActivity
 */

// BitmapFactory.decodeStream(assets.open(path)) equivalent: RGBA8888 pixels
static uint8_t * image_load(const char * path, int * w, int * h) {
    AAsset * a = AAssetManager_open(NULL, path, 0);
    if (!a) return NULL;
    int comp;
    uint8_t * px = stbi_load_from_memory(AAsset_getBuffer(a), (int) AAsset_getLength(a), w, h, &comp, 4);
    AAsset_close(a);
    if (!px) l_error("ImageGet*(\"%s\"): decode failed: %s", path, stbi_failure_reason());
    return px;
}

jint MainActivity_ImageGetWidth(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    int w = 0, h = 0;
    uint8_t * px = image_load(path, &w, &h);
    stbi_image_free(px);
    l_info("MainActivity.ImageGetWidth(\"%s\"): %i", path, w);
    jstr_release(_path, path);
    return w;
}

jint MainActivity_ImageGetHeight(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    int w = 0, h = 0;
    uint8_t * px = image_load(path, &w, &h);
    stbi_image_free(px);
    l_info("MainActivity.ImageGetHeight(\"%s\"): %i", path, h);
    jstr_release(_path, path);
    return h;
}

jobject MainActivity_ImageGetData(jmethodID id, va_list args) {
    jstring _path = va_arg(args, jstring);
    const char * path = jstr_get(_path);
    int w = 0, h = 0;
    uint8_t * px = image_load(path, &w, &h);
    JavaDynArray * jda = NULL;

    if (px) {
        // Bitmap.getPixels(): one 0xAARRGGBB int per pixel
        jda = jda_alloc(w * h, FIELD_TYPE_INT);
        jint * out = (jint *) jda->array;
        for (int i = 0; i < w * h; i++) {
            const uint8_t * p = px + i * 4;
            out[i] = (jint) (((uint32_t) p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2]);
        }
        stbi_image_free(px);
    }

    l_info("MainActivity.ImageGetData(\"%s\"): %ix%i", path, w, h);
    jstr_release(_path, path);
    return (jobject) jda;
}

void MainActivity_postErrorMessage(jmethodID id, va_list args) {
    jint err = va_arg(args, jint);
    l_error("MainActivity.postErrorMessage(%i)", err);
}

void MainActivity_postDeleteSaveFile(jmethodID id, va_list args) {
    jint num = va_arg(args, jint);
    // TODO (Fase 6): confirmation dialog, then delete save%02d.ff7 and call updateSaveDataFile()
    l_warn("MainActivity.postDeleteSaveFile(%i): not implemented", num);
}

void MainActivity_OpenURL(jmethodID id, va_list args) {
    jstring _url = va_arg(args, jstring);
    const char * url = jstr_get(_url);
    l_info("MainActivity.OpenURL(\"%s\"): ignored", url);
    jstr_release(_url, url);
}

void MainActivity_CloudLaunch(jmethodID id, va_list args) {
    l_info("MainActivity.CloudLaunch(): ignored");
}

/*
 * jp.co.d4e.materialg.SEPlayer (4 UI sounds from res/raw) — Fase 3
 */

void SEPlayer_PLAY(jmethodID id, va_list args) {
    jint se = va_arg(args, jint);
    l_debug("SEPlayer.PLAY(%i)", se);
}

void SEPlayer_SETVOLUME(jmethodID id, va_list args) {
    double vol = va_arg(args, double); // floats are promoted through varargs
    l_debug("SEPlayer.SETVOLUME(%f)", vol);
}

/*
 * jp.co.d4e.materialg.MyDecoder (WebM FMV player) — Fase 5.
 * START() reports failure for now so the game skips the movies.
 */

jint MyDecoder_START(jmethodID id, va_list args) {
    jstring _fname = va_arg(args, jstring);
    const char * fname = jstr_get(_fname);
    l_warn("MyDecoder.START(\"%s\"): video not implemented yet", fname);
    jstr_release(_fname, fname);
    return 0;
}

void MyDecoder_SET_TEXTURE(jmethodID id, va_list args) {
    jint tex = va_arg(args, jint);
    l_debug("MyDecoder.SET_TEXTURE(%i)", tex);
}

jint MyDecoder_FRAME(jmethodID id, va_list args) {
    return 0;
}

void MyDecoder_AFTER_FRAME(jmethodID id, va_list args) {
}

void MyDecoder_RESET(jmethodID id, va_list args) {
    l_debug("MyDecoder.RESET()");
}

void MyDecoder_SET_VOLUME(jmethodID id, va_list args) {
    double vol = va_arg(args, double);
    l_debug("MyDecoder.SET_VOLUME(%f)", vol);
}

jint MyDecoder_GET_TOTALTIME(jmethodID id, va_list args) {
    return 0;
}

jint MyDecoder_GET_POSITION(jmethodID id, va_list args) {
    return 0;
}

void MyDecoder_SET_POSITION(jmethodID id, va_list args) {
    jint pos = va_arg(args, jint);
    l_debug("MyDecoder.SET_POSITION(%i)", pos);
}

/*
 * jp.co.d4e.materialg.RendererWrapper
 */

jboolean RendererWrapper_GET_YUV_PATCH(jmethodID id, va_list args) {
    return JNI_FALSE; // device-specific Samsung workaround on Android
}

/*
 * JNI Methods
 */

NameToMethodID nameToMethodId[] = {
    { 100, "OPEN_FILE_DESCRIPTOR",  METHOD_TYPE_INT },
    { 101, "CLOSE_FILE_DESCRIPTOR", METHOD_TYPE_VOID },
    { 102, "GET_START_OFFSET",      METHOD_TYPE_LONG },
    { 103, "GET_LENGTH",            METHOD_TYPE_LONG },

    { 200, "ImageGetData",          METHOD_TYPE_OBJECT },
    { 201, "ImageGetWidth",         METHOD_TYPE_INT },
    { 202, "ImageGetHeight",        METHOD_TYPE_INT },
    { 203, "postErrorMessage",      METHOD_TYPE_VOID },
    { 204, "postDeleteSaveFile",    METHOD_TYPE_VOID },
    { 205, "OpenURL",               METHOD_TYPE_VOID },
    { 206, "CloudLaunch",           METHOD_TYPE_VOID },

    { 300, "PLAY",                  METHOD_TYPE_VOID },
    { 301, "SETVOLUME",             METHOD_TYPE_VOID },

    { 400, "START",                 METHOD_TYPE_INT },
    { 401, "SET_TEXTURE",           METHOD_TYPE_VOID },
    { 402, "FRAME",                 METHOD_TYPE_INT },
    { 403, "AFTER_FRAME",           METHOD_TYPE_VOID },
    { 404, "RESET",                 METHOD_TYPE_VOID },
    { 405, "SET_VOLUME",            METHOD_TYPE_VOID },
    { 406, "GET_TOTALTIME",         METHOD_TYPE_INT },
    { 407, "GET_POSITION",          METHOD_TYPE_INT },
    { 408, "SET_POSITION",          METHOD_TYPE_VOID },

    { 500, "GET_YUV_PATCH",         METHOD_TYPE_BOOLEAN },
};

MethodsBoolean methodsBoolean[] = {
    { 500, RendererWrapper_GET_YUV_PATCH },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { 100, ExpansionFile_OPEN_FILE_DESCRIPTOR },
    { 201, MainActivity_ImageGetWidth },
    { 202, MainActivity_ImageGetHeight },
    { 400, MyDecoder_START },
    { 402, MyDecoder_FRAME },
    { 406, MyDecoder_GET_TOTALTIME },
    { 407, MyDecoder_GET_POSITION },
};
MethodsLong methodsLong[] = {
    { 102, ExpansionFile_GET_START_OFFSET },
    { 103, ExpansionFile_GET_LENGTH },
};
MethodsObject methodsObject[] = {
    { 200, MainActivity_ImageGetData },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 101, ExpansionFile_CLOSE_FILE_DESCRIPTOR },
    { 203, MainActivity_postErrorMessage },
    { 204, MainActivity_postDeleteSaveFile },
    { 205, MainActivity_OpenURL },
    { 206, MainActivity_CloudLaunch },
    { 300, SEPlayer_PLAY },
    { 301, SEPlayer_SETVOLUME },
    { 401, MyDecoder_SET_TEXTURE },
    { 403, MyDecoder_AFTER_FRAME },
    { 404, MyDecoder_RESET },
    { 405, MyDecoder_SET_VOLUME },
    { 408, MyDecoder_SET_POSITION },
};

/*
 * JNI Fields
 */

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
