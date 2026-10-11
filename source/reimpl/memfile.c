/*
 * In-memory files for the game's scratch files.
 *
 * On every battle (and at boot) the game rewrites 16 small files in its
 * Documents folder (TF2D.P ... SGT3D.P, < 400 bytes each) and reads them
 * straight back: 32 opens plus writes and reads on the memory card, which
 * profiling showed as a large part of the battle-load stutter.
 *
 * Paths ending in ".P" are kept in RAM instead. A file opened for the first
 * time without O_TRUNC is loaded from the card if it exists, so the
 * behaviour is the same as with real files; writes are not persisted (the
 * game regenerates these files before using them).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/memfile.h"

#include "utils/logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#define MAX_FILES   32
#define MAX_HANDLES 32
#define FD_BASE     0x5000

#define B_O_ACCMODE 03
#define B_O_CREAT   0100
#define B_O_TRUNC   01000
#define B_O_APPEND  02000

typedef struct {
    char path[128];
    uint8_t * data;
    uint32_t size, cap;
} mfile;

static mfile files[MAX_FILES];
static int num_files;
static struct {
    int used;
    int file;
    uint32_t pos;
    int append;
} handles[MAX_HANDLES];
static SceKernelLwMutexWork lock;
static int lock_ready;
static unsigned opens;

int memfile_handles(const char * path) {
    size_t n = strlen(path);
    return n > 2 && !strcmp(path + n - 2, ".P") && strstr(path, "Documents/") != NULL;
}

int memfile_is(int fd) {
    return fd >= FD_BASE && fd < FD_BASE + MAX_HANDLES;
}

static int ensure_cap(mfile * f, uint32_t need) {
    if (need <= f->cap) return 1;
    uint32_t cap = f->cap ? f->cap : 512;
    while (cap < need) cap *= 2;
    uint8_t * d = realloc(f->data, cap);
    if (!d) return 0;
    f->data = d;
    f->cap = cap;
    return 1;
}

// Loads an existing card file into a new entry; -1 if it does not exist
static int load_from_card(const char * path) {
    FILE * fp = fopen(path, "rb");
    if (!fp) return -1;
    int i = num_files++;
    strncpy(files[i].path, path, sizeof(files[i].path) - 1);
    uint8_t buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (!ensure_cap(&files[i], files[i].size + r)) break;
        memcpy(files[i].data + files[i].size, buf, r);
        files[i].size += r;
    }
    fclose(fp);
    return i;
}

int memfile_open(const char * path, int bionic_flags) {
    if (!lock_ready) {
        sceKernelCreateLwMutex(&lock, "memfile", 0, 0, NULL);
        lock_ready = 1;
    }
    sceKernelLockLwMutex(&lock, 1, NULL);
    int i = 0;
    while (i < num_files && strcmp(files[i].path, path)) i++;
    if (i == num_files) {
        if (num_files == MAX_FILES) {
            sceKernelUnlockLwMutex(&lock, 1);
            return -2; // full: let the caller use the card
        }
        if (!(bionic_flags & B_O_TRUNC)) i = load_from_card(path);
        if (i < 0) {
            if (!(bionic_flags & B_O_CREAT)) {
                sceKernelUnlockLwMutex(&lock, 1);
                errno = ENOENT;
                return -1;
            }
            i = num_files++;
            strncpy(files[i].path, path, sizeof(files[i].path) - 1);
        }
    }
    if (bionic_flags & B_O_TRUNC) files[i].size = 0;

    int h = 0;
    while (h < MAX_HANDLES && handles[h].used) h++;
    if (h == MAX_HANDLES) {
        sceKernelUnlockLwMutex(&lock, 1);
        return -2;
    }
    handles[h].used = 1;
    handles[h].file = i;
    handles[h].pos = 0;
    handles[h].append = (bionic_flags & B_O_APPEND) != 0;
    if (!opens++) l_info("memfile: %s kept in RAM (and every other *.P scratch file)", path);
    sceKernelUnlockLwMutex(&lock, 1);
    return FD_BASE + h;
}

int memfile_close(int fd) {
    handles[fd - FD_BASE].used = 0;
    return 0;
}

ssize_t memfile_read(int fd, void * buf, size_t count) {
    int h = fd - FD_BASE;
    if (!handles[h].used) { errno = EBADF; return -1; }
    mfile * f = &files[handles[h].file];
    uint32_t pos = handles[h].pos;
    if (pos >= f->size) return 0;
    if (count > f->size - pos) count = f->size - pos;
    memcpy(buf, f->data + pos, count);
    handles[h].pos = pos + count;
    return (ssize_t) count;
}

ssize_t memfile_write(int fd, const void * buf, size_t count) {
    int h = fd - FD_BASE;
    if (!handles[h].used) { errno = EBADF; return -1; }
    mfile * f = &files[handles[h].file];
    uint32_t pos = handles[h].append ? f->size : handles[h].pos;
    if (!ensure_cap(f, pos + count)) { errno = ENOSPC; return -1; }
    if (pos > f->size) memset(f->data + f->size, 0, pos - f->size);
    memcpy(f->data + pos, buf, count);
    if (pos + count > f->size) f->size = pos + count;
    handles[h].pos = pos + count;
    return (ssize_t) count;
}

int64_t memfile_lseek(int fd, int64_t offset, int whence) {
    int h = fd - FD_BASE;
    if (!handles[h].used) { errno = EBADF; return -1; }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? handles[h].pos : files[handles[h].file].size;
    int64_t pos = base + offset;
    if (pos < 0 || pos > 0x7fffffff) { errno = EINVAL; return -1; }
    handles[h].pos = (uint32_t) pos;
    return pos;
}

uint32_t memfile_size(int fd) {
    int h = fd - FD_BASE;
    return handles[h].used ? files[handles[h].file].size : 0;
}
