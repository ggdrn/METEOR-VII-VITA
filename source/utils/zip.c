/*
 * Minimal read-only ZIP reader (stored + deflate, no zip64).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/zip.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <zlib.h>

#define EOCD_SIG          0x06054b50
#define CDIR_SIG          0x02014b50
#define LOCAL_SIG         0x04034b50
#define EOCD_MIN_SIZE     22
#define EOCD_MAX_COMMENT  0xFFFF

static inline uint16_t rd16(const uint8_t * p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t * p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int read_at(int fd, int64_t off, void * buf, size_t len) {
    if (sceIoLseek(fd, off, SCE_SEEK_SET) != off) return -1;
    uint8_t * dst = buf;
    while (len > 0) {
        int r = sceIoRead(fd, dst, len);
        if (r <= 0) return -1;
        dst += r;
        len -= r;
    }
    return 0;
}

zip_archive * zip_open(const char * path) {
    int fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        l_error("zip_open: could not open %s (0x%08x)", path, fd);
        return NULL;
    }

    int64_t file_size = sceIoLseek(fd, 0, SCE_SEEK_END);
    size_t tail_size = (file_size < EOCD_MIN_SIZE + EOCD_MAX_COMMENT)
                       ? (size_t)file_size : EOCD_MIN_SIZE + EOCD_MAX_COMMENT;
    uint8_t * tail = malloc(tail_size);
    zip_archive * zip = NULL;
    uint8_t * cdir = NULL;

    if (!tail || read_at(fd, file_size - tail_size, tail, tail_size) < 0)
        goto fail;

    // Scan backwards for the End Of Central Directory record
    const uint8_t * eocd = NULL;
    for (int i = (int)tail_size - EOCD_MIN_SIZE; i >= 0; i--) {
        if (rd32(tail + i) == EOCD_SIG) { eocd = tail + i; break; }
    }
    if (!eocd) {
        l_error("zip_open: %s is not a zip file", path);
        goto fail;
    }

    uint16_t num = rd16(eocd + 10);
    uint32_t cdir_size = rd32(eocd + 12);
    uint32_t cdir_off = rd32(eocd + 16);

    cdir = malloc(cdir_size);
    if (!cdir || read_at(fd, cdir_off, cdir, cdir_size) < 0)
        goto fail;

    zip = calloc(1, sizeof(zip_archive));
    zip->fd = fd;
    zip->entries = calloc(num, sizeof(zip_entry));

    const uint8_t * p = cdir;
    const uint8_t * end = cdir + cdir_size;
    for (int i = 0; i < num; i++) {
        if (p + 46 > end || rd32(p) != CDIR_SIG) {
            l_error("zip_open: corrupt central directory in %s", path);
            goto fail;
        }
        uint16_t name_len = rd16(p + 28);
        uint16_t extra_len = rd16(p + 30);
        uint16_t comment_len = rd16(p + 32);

        zip_entry * e = &zip->entries[i];
        e->method = rd16(p + 10);
        e->comp_size = rd32(p + 20);
        e->uncomp_size = rd32(p + 24);
        e->local_header_offset = rd32(p + 42);
        e->data_offset = -1;
        e->name = malloc(name_len + 1);
        memcpy(e->name, p + 46, name_len);
        e->name[name_len] = '\0';

        zip->num_entries++;
        p += 46 + name_len + extra_len + comment_len;
    }

    free(cdir);
    free(tail);
    l_info("zip_open: %s (%d entries)", path, zip->num_entries);
    return zip;

fail:
    free(cdir);
    free(tail);
    if (zip) {
        zip->fd = -1;
        zip_close(zip);
    }
    sceIoClose(fd);
    return NULL;
}

void zip_close(zip_archive * zip) {
    if (!zip) return;
    for (int i = 0; i < zip->num_entries; i++)
        free(zip->entries[i].name);
    free(zip->entries);
    if (zip->fd >= 0)
        sceIoClose(zip->fd);
    free(zip);
}

zip_entry * zip_find(zip_archive * zip, const char * name) {
    for (int i = 0; i < zip->num_entries; i++) {
        if (strcmp(zip->entries[i].name, name) == 0)
            return &zip->entries[i];
    }
    return NULL;
}

int64_t zip_entry_data_offset(zip_archive * zip, zip_entry * entry) {
    if (entry->data_offset >= 0)
        return entry->data_offset;

    // Name/extra lengths in the local header may differ from the central one
    uint8_t hdr[30];
    if (read_at(zip->fd, entry->local_header_offset, hdr, sizeof(hdr)) < 0 || rd32(hdr) != LOCAL_SIG)
        return -1;

    entry->data_offset = (int64_t)entry->local_header_offset + 30 + rd16(hdr + 26) + rd16(hdr + 28);
    return entry->data_offset;
}

void * zip_read_entry(zip_archive * zip, zip_entry * entry, size_t * out_size) {
    int64_t off = zip_entry_data_offset(zip, entry);
    if (off < 0) return NULL;

    uint8_t * out = malloc(entry->uncomp_size ? entry->uncomp_size : 1);
    if (!out) return NULL;

    if (entry->method == 0) {
        if (read_at(zip->fd, off, out, entry->uncomp_size) < 0)
            goto fail;
    } else if (entry->method == 8) {
        uint8_t * comp = malloc(entry->comp_size);
        if (!comp || read_at(zip->fd, off, comp, entry->comp_size) < 0) {
            free(comp);
            goto fail;
        }

        z_stream zs = {0};
        zs.next_in = comp;
        zs.avail_in = entry->comp_size;
        zs.next_out = out;
        zs.avail_out = entry->uncomp_size;
        int ok = inflateInit2(&zs, -MAX_WBITS) == Z_OK
                 && inflate(&zs, Z_FINISH) == Z_STREAM_END
                 && zs.total_out == entry->uncomp_size;
        inflateEnd(&zs);
        free(comp);
        if (!ok) {
            l_error("zip_read_entry: inflate failed for %s", entry->name);
            goto fail;
        }
    } else {
        l_error("zip_read_entry: unsupported method %d for %s", entry->method, entry->name);
        goto fail;
    }

    if (out_size) *out_size = entry->uncomp_size;
    return out;

fail:
    free(out);
    return NULL;
}
