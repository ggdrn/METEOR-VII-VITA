/*
 * Minimal read-only ZIP reader (stored + deflate, no zip64).
 *
 * Used to read the original game files in place, without extracting them:
 *  - base.apk -> libjni_ff7.so and assets/ (AAssetManager)
 *  - .obb     -> stored entries, served to the game as (fd, offset, length)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_ZIP_H
#define SOLOADER_ZIP_H

#include <stdint.h>
#include <stddef.h>

#include <psp2/kernel/threadmgr.h>

typedef struct zip_entry {
    char * name;
    uint16_t method;          // 0 = stored, 8 = deflate
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_header_offset;
    int64_t data_offset;      // resolved lazily, -1 until known
} zip_entry;

typedef struct zip_archive {
    int fd;                   // SceUID, shared: every seek+read holds `lock`
    SceKernelLwMutexWork lock;
    zip_entry * entries;
    int num_entries;
} zip_archive;

/** Opens an archive and reads its central directory. Returns NULL on error. */
zip_archive * zip_open(const char * path);

void zip_close(zip_archive * zip);

/** Exact-name lookup. Returns NULL if not found. */
zip_entry * zip_find(zip_archive * zip, const char * name);

/**
 * Reads `len` bytes at absolute offset `off` of the archive file.
 * Thread-safe. Returns 0 on success, -1 on error.
 */
int zip_read_raw(zip_archive * zip, int64_t off, void * buf, size_t len);

/** Absolute file offset of the entry's (possibly compressed) data, or -1. */
int64_t zip_entry_data_offset(zip_archive * zip, zip_entry * entry);

/**
 * Reads and decompresses a whole entry into a malloc'd buffer.
 * Returns NULL on error. `out_size` (optional) receives the uncompressed size.
 */
void * zip_read_entry(zip_archive * zip, zip_entry * entry, size_t * out_size);

#endif // SOLOADER_ZIP_H
