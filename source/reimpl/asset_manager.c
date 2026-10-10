/*
 * AAssetManager reimplementation backed by the original APK.
 *
 * Assets are served straight from `assets/` inside base.apk (kept open in
 * `apk_zip`), so nothing has to be extracted. Small assets are read into
 * memory on open; `AAsset_openFileDescriptor*` returns a descriptor of the
 * APK itself plus the entry's offset, exactly like Android does for
 * uncompressed assets.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/asset_manager.h"

#include "utils/logger.h"
#include "utils/zip.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern zip_archive * apk_zip;

struct AAsset {
    zip_entry * entry;
    uint8_t * data;
    size_t size;
    size_t pos;
};

// Any non-NULL value works: the game only passes it back to us
static int g_asset_manager_dummy;

AAssetManager * AAssetManager_create() {
    return (AAssetManager *) &g_asset_manager_dummy;
}

AAssetManager * AAssetManager_fromJava(void * env, void * assetManager) {
    return AAssetManager_create();
}

static zip_entry * find_asset(const char * filename) {
    char path[512];
    while (filename[0] == '/') filename++;
    snprintf(path, sizeof(path), "assets/%s", filename);
    return apk_zip ? zip_find(apk_zip, path) : NULL;
}

AAsset * AAssetManager_open(AAssetManager * mgr, const char * filename, int mode) {
    zip_entry * e = find_asset(filename);
    if (!e) {
        l_warn("AAssetManager_open(\"%s\"): not found in APK", filename);
        return NULL;
    }

    AAsset * a = calloc(1, sizeof(AAsset));
    a->entry = e;
    a->data = zip_read_entry(apk_zip, e, &a->size);
    if (!a->data) {
        l_error("AAssetManager_open(\"%s\"): read failed", filename);
        free(a);
        return NULL;
    }

    l_info("AAssetManager_open(\"%s\", %i): %u bytes", filename, mode, a->size);
    return a;
}

void AAsset_close(AAsset * asset) {
    if (!asset) return;
    free(asset->data);
    free(asset);
}

int AAsset_read(AAsset * asset, void * buf, size_t count) {
    if (!asset) return -1;
    size_t left = asset->size - asset->pos;
    if (count > left) count = left;
    memcpy(buf, asset->data + asset->pos, count);
    asset->pos += count;
    return (int) count;
}

int64_t AAsset_seek64(AAsset * asset, int64_t offset, int whence) {
    if (!asset) return -1;
    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t) asset->pos; break;
        case SEEK_END: base = (int64_t) asset->size; break;
        default: return -1;
    }
    int64_t target = base + offset;
    if (target < 0 || target > (int64_t) asset->size) return -1;
    asset->pos = (size_t) target;
    return target;
}

off_t AAsset_seek(AAsset * asset, off_t offset, int whence) {
    return (off_t) AAsset_seek64(asset, offset, whence);
}

off_t AAsset_getLength(AAsset * asset) {
    return asset ? (off_t) asset->size : -1;
}

int64_t AAsset_getLength64(AAsset * asset) {
    return asset ? (int64_t) asset->size : -1;
}

off_t AAsset_getRemainingLength(AAsset * asset) {
    return asset ? (off_t) (asset->size - asset->pos) : -1;
}

int64_t AAsset_getRemainingLength64(AAsset * asset) {
    return asset ? (int64_t) (asset->size - asset->pos) : -1;
}

const void * AAsset_getBuffer(AAsset * asset) {
    return asset ? asset->data : NULL;
}

int AAsset_openFileDescriptor64(AAsset * asset, int64_t * outStart, int64_t * outLength) {
    if (!asset || asset->entry->method != 0) {
        // Android also refuses compressed assets here
        l_warn("AAsset_openFileDescriptor64(%p): not possible", asset);
        return -1;
    }

    int fd = open(APK_PATH, O_RDONLY);
    if (fd < 0) {
        l_error("AAsset_openFileDescriptor64: could not open %s", APK_PATH);
        return -1;
    }

    if (outStart) *outStart = zip_entry_data_offset(apk_zip, asset->entry);
    if (outLength) *outLength = asset->entry->uncomp_size;
    l_info("AAsset_openFileDescriptor64(\"%s\"): fd %i, start %lld, len %u",
           asset->entry->name, fd, outStart ? *outStart : -1LL, asset->entry->uncomp_size);
    return fd;
}

int AAsset_openFileDescriptor(AAsset * asset, off_t * outStart, off_t * outLength) {
    int64_t start, len;
    int fd = AAsset_openFileDescriptor64(asset, &start, &len);
    if (outStart) *outStart = (off_t) start;
    if (outLength) *outLength = (off_t) len;
    return fd;
}

AAssetDir * AAssetManager_openDir(AAssetManager * mgr, const char * dirName) {
    l_warn("UNIMPLEMENTED: AAssetManager_openDir(\"%s\")", dirName);
    return (AAssetDir *) strdup("dummy");
}

const char * AAssetDir_getNextFileName(AAssetDir * assetDir) {
    return NULL;
}

void AAssetDir_close(AAssetDir * assetDir) {
    free(assetDir);
}
