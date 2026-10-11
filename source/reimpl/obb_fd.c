/*
 * Buffered file descriptors over the original OBB.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/obb_fd.h"

#include "utils/logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#define OBB_BUF_SIZE (256 * 1024)
// Log read statistics every this many bytes per descriptor
#define OBB_STATS_STEP (16 * 1024 * 1024)

typedef struct {
    bool used;
    SceUID uid;
    char tag[96];
    int64_t pos;          // logical position seen by the game
    int64_t size;         // whole OBB size
    uint8_t * buf;
    int64_t buf_off;      // file offset of buf[0]
    int buf_len;          // valid bytes in buf
    // statistics
    uint64_t bytes;
    uint32_t calls;
    uint32_t refills;
    uint64_t io_us;
    uint64_t next_report;
    SceKernelLwMutexWork lock;
} obb_fd_t;

static obb_fd_t fds[OBB_FD_MAX];
static SceKernelLwMutexWork table_lock;
static bool table_lock_ready;

static obb_fd_t * get(int fd) {
    int i = fd - OBB_FD_BASE;
    if (i < 0 || i >= OBB_FD_MAX || !fds[i].used) return NULL;
    return &fds[i];
}

bool obb_fd_is(int fd) {
    return get(fd) != NULL;
}

int obb_fd_open(const char * tag) {
    if (!table_lock_ready) {
        sceKernelCreateLwMutex(&table_lock, "obb_fd_table", 0, 0, NULL);
        table_lock_ready = true;
    }

    SceUID uid = sceIoOpen(OBB_PATH, SCE_O_RDONLY, 0);
    if (uid < 0) {
        l_error("obb_fd_open: sceIoOpen(%s) failed: 0x%08x", OBB_PATH, uid);
        return -1;
    }

    sceKernelLockLwMutex(&table_lock, 1, NULL);
    int fd = -1;
    for (int i = 0; i < OBB_FD_MAX; i++) {
        if (!fds[i].used) {
            obb_fd_t * f = &fds[i];
            memset(f, 0, sizeof(*f));
            f->used = true;
            f->uid = uid;
            f->size = sceIoLseek(uid, 0, SCE_SEEK_END);
            f->buf = malloc(OBB_BUF_SIZE);
            f->next_report = OBB_STATS_STEP;
            strncpy(f->tag, tag ? tag : "?", sizeof(f->tag) - 1);
            sceKernelCreateLwMutex(&f->lock, "obb_fd", 0, 0, NULL);
            fd = OBB_FD_BASE + i;
            break;
        }
    }
    sceKernelUnlockLwMutex(&table_lock, 1);

    if (fd < 0) {
        l_error("obb_fd_open: out of descriptors (%d)", OBB_FD_MAX);
        sceIoClose(uid);
    }
    return fd;
}

// Reads at an absolute offset, bypassing the buffer.
static int raw_read(obb_fd_t * f, int64_t off, void * dst, size_t len) {
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = sceIoPread(f->uid, dst, len, off);
    f->io_us += sceKernelGetProcessTimeWide() - t0;
    return r;
}

static void report(obb_fd_t * f, const char * why) {
    l_info("OBB read stats [%s] %s: %llu KB in %u calls (%u refills), %llu ms in IO",
           f->tag, why, f->bytes / 1024, f->calls, f->refills, f->io_us / 1000);
}

ssize_t obb_fd_read(int fd, void * buf, size_t count) {
    obb_fd_t * f = get(fd);
    if (!f) { errno = EBADF; return -1; }

    sceKernelLockLwMutex(&f->lock, 1, NULL);
    uint8_t * dst = buf;
    size_t done = 0;

    if (f->pos + (int64_t) count > f->size)
        count = (size_t) (f->size > f->pos ? f->size - f->pos : 0);

    while (done < count) {
        int64_t pos = f->pos + done;
        size_t left = count - done;

        // Serve from the buffer when possible
        if (pos >= f->buf_off && pos < f->buf_off + f->buf_len) {
            size_t n = (size_t) (f->buf_off + f->buf_len - pos);
            if (n > left) n = left;
            memcpy(dst + done, f->buf + (pos - f->buf_off), n);
            done += n;
            continue;
        }

        // Large reads go straight to the destination
        if (left >= OBB_BUF_SIZE) {
            int r = raw_read(f, pos, dst + done, left);
            if (r <= 0) break;
            done += r;
            continue;
        }

        // Refill
        int r = raw_read(f, pos, f->buf, OBB_BUF_SIZE);
        f->refills++;
        if (r <= 0) { f->buf_len = 0; break; }
        f->buf_off = pos;
        f->buf_len = r;
    }

    f->pos += done;
    f->bytes += done;
    f->calls++;
    if (f->bytes >= f->next_report) {
        report(f, "progress");
        f->next_report += OBB_STATS_STEP;
    }
    sceKernelUnlockLwMutex(&f->lock, 1);
    return (ssize_t) done;
}

int64_t obb_fd_lseek(int fd, int64_t offset, int whence) {
    obb_fd_t * f = get(fd);
    if (!f) { errno = EBADF; return -1; }

    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = f->pos; break;
        case SEEK_END: base = f->size; break;
        default: errno = EINVAL; return -1;
    }
    if (base + offset < 0) { errno = EINVAL; return -1; }
    f->pos = base + offset;
    return f->pos;
}

int obb_fd_close(int fd) {
    obb_fd_t * f = get(fd);
    if (!f) { errno = EBADF; return -1; }

    if (f->calls) report(f, "close");
    sceKernelLockLwMutex(&table_lock, 1, NULL);
    sceIoClose(f->uid);
    free(f->buf);
    sceKernelDeleteLwMutex(&f->lock);
    f->used = false;
    sceKernelUnlockLwMutex(&table_lock, 1);
    return 0;
}
