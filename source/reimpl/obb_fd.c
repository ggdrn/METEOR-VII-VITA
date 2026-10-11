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
// Small reads go through a block cache shared by every descriptor. Some
// regions of the OBB read at only 0.3-0.9 MB/s on the card (battle.lgp,
// magic.lgp, char.lgp...), and every battle re-reads the same effect data.
#define BLOCK_SIZE (64 * 1024)
#define BLOCK_SLOTS 512 // 32 MB

// Whole-file reads (music .akb, etc.) are kept in RAM: they repeat often
// (battle music, area music) and some took over 2 s to read from the card
#define BIG_READ_MIN (512 * 1024)
#define BIG_CACHE_MAX (48 * 1024 * 1024)
#define BIG_CACHE_SLOTS 32
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

// Totals for the heartbeat (all descriptors)
static volatile uint64_t total_io_us, total_bytes;
#define SLOW_READ_US 30000

typedef struct {
    int64_t off;
    size_t len;
    uint8_t * data;
    uint32_t last_use;
} big_entry_t;

typedef struct {
    int64_t index;        // block number (offset / BLOCK_SIZE), -1 if free
    uint32_t len;
    uint32_t last_use;
    uint8_t * data;
} block_t;

static block_t blocks[BLOCK_SLOTS];
static uint32_t block_clock;
static SceKernelLwMutexWork block_lock;
static volatile uint32_t block_hits, block_misses;

static big_entry_t big_cache[BIG_CACHE_SLOTS];
static size_t big_cache_bytes;
static uint32_t big_cache_clock;
static SceKernelLwMutexWork big_cache_lock;
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
        sceKernelCreateLwMutex(&big_cache_lock, "obb_big_cache", 0, 0, NULL);
        sceKernelCreateLwMutex(&block_lock, "obb_blocks", 0, 0, NULL);
        for (int i = 0; i < BLOCK_SLOTS; i++) blocks[i].index = -1;
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

// Copies the 64 KB block `index` into `dst`. Returns its length or <= 0.
static int block_read(obb_fd_t * f, int64_t index, uint8_t * dst) {
    sceKernelLockLwMutex(&block_lock, 1, NULL);
    for (int i = 0; i < BLOCK_SLOTS; i++) {
        if (blocks[i].index == index) {
            int len = blocks[i].len;
            memcpy(dst, blocks[i].data, len);
            blocks[i].last_use = ++block_clock;
            block_hits++;
            sceKernelUnlockLwMutex(&block_lock, 1);
            return len;
        }
    }
    sceKernelUnlockLwMutex(&block_lock, 1);

    // Miss: read outside the lock so other threads aren't held up by the card
    int len = raw_read(f, index * BLOCK_SIZE, dst, BLOCK_SIZE);
    if (len <= 0) return len;
    block_misses++;

    sceKernelLockLwMutex(&block_lock, 1, NULL);
    int slot = -1;
    for (int i = 0; i < BLOCK_SLOTS; i++) {
        if (blocks[i].index == index) { slot = -2; break; } // another thread got it first
        if (slot == -1 || (blocks[slot].index != -1 && (blocks[i].index == -1 || blocks[i].last_use < blocks[slot].last_use)))
            slot = i;
    }
    if (slot >= 0) {
        if (!blocks[slot].data) blocks[slot].data = malloc(BLOCK_SIZE);
        if (blocks[slot].data) {
            memcpy(blocks[slot].data, dst, len);
            blocks[slot].index = index;
            blocks[slot].len = len;
            blocks[slot].last_use = ++block_clock;
        }
    }
    sceKernelUnlockLwMutex(&block_lock, 1);
    return len;
}

// Large reads: served from / stored in the RAM cache. Returns bytes read.
static int big_read(obb_fd_t * f, int64_t off, uint8_t * dst, size_t len) {
    sceKernelLockLwMutex(&big_cache_lock, 1, NULL);
    for (int i = 0; i < BIG_CACHE_SLOTS; i++) {
        if (big_cache[i].data && big_cache[i].off == off && big_cache[i].len == len) {
            memcpy(dst, big_cache[i].data, len);
            big_cache[i].last_use = ++big_cache_clock;
            sceKernelUnlockLwMutex(&big_cache_lock, 1);
            l_info("OBB [%s]: %u KB from RAM cache", f->tag, len / 1024);
            return (int) len;
        }
    }
    sceKernelUnlockLwMutex(&big_cache_lock, 1);

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int r = raw_read(f, off, dst, len);
    SceUInt64 us = sceKernelGetProcessTimeWide() - t0;
    l_info("OBB [%s]: read %u KB at offset %lld in %llu ms (%.1f MB/s)", f->tag, len / 1024, off, us / 1000,
           us ? len / (double) us : 0.0);
    if (r != (int) len || len > BIG_CACHE_MAX / 2) return r;

    sceKernelLockLwMutex(&big_cache_lock, 1, NULL);
    // Evict least recently used entries until it fits
    while (big_cache_bytes + len > BIG_CACHE_MAX) {
        int lru = -1;
        for (int i = 0; i < BIG_CACHE_SLOTS; i++)
            if (big_cache[i].data && (lru < 0 || big_cache[i].last_use < big_cache[lru].last_use)) lru = i;
        if (lru < 0) break;
        big_cache_bytes -= big_cache[lru].len;
        free(big_cache[lru].data);
        big_cache[lru].data = NULL;
    }
    int slot = -1;
    for (int i = 0; i < BIG_CACHE_SLOTS && slot < 0; i++)
        if (!big_cache[i].data) slot = i;
    if (slot < 0) { // all slots used: replace the least recently used
        slot = 0;
        for (int i = 1; i < BIG_CACHE_SLOTS; i++)
            if (big_cache[i].last_use < big_cache[slot].last_use) slot = i;
        big_cache_bytes -= big_cache[slot].len;
        free(big_cache[slot].data);
        big_cache[slot].data = NULL;
    }
    uint8_t * copy = malloc(len);
    if (copy) {
        memcpy(copy, dst, len);
        big_cache[slot] = (big_entry_t) { off, len, copy, ++big_cache_clock };
        big_cache_bytes += len;
    }
    sceKernelUnlockLwMutex(&big_cache_lock, 1);
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
    SceUInt64 io_before = f->io_us;
    SceUInt64 t_call = sceKernelGetProcessTimeWide();
    int64_t pos_call = f->pos;
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

        // Large reads go straight to the destination (through the RAM cache)
        if (left >= OBB_BUF_SIZE) {
            int r = left >= BIG_READ_MIN ? big_read(f, pos, dst + done, left)
                                         : raw_read(f, pos, dst + done, left);
            if (r <= 0) break;
            done += r;
            continue;
        }

        // Refill from the shared block cache
        int64_t index = pos / BLOCK_SIZE;
        int r = block_read(f, index, f->buf);
        f->refills++;
        if (r <= 0) { f->buf_len = 0; break; }
        f->buf_off = index * BLOCK_SIZE;
        f->buf_len = r;
        if (pos >= f->buf_off + f->buf_len) break; // past the end of the file
    }

    f->pos += done;
    f->bytes += done;
    f->calls++;

    SceUInt64 call_us = sceKernelGetProcessTimeWide() - t_call;
    total_io_us += f->io_us - io_before;
    total_bytes += done;
    if (call_us > SLOW_READ_US)
        l_warn("OBB slow read [%s]: %u bytes at file+%lld took %llu ms (%llu ms in IO)",
               f->tag, done, pos_call, call_us / 1000, (f->io_us - io_before) / 1000);
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

void obb_fd_report(double seconds) {
    if (seconds <= 0) return;
    l_info("obb/s: %.0f KB read, %.1f ms in IO | block cache: %u hits, %u misses",
           total_bytes / 1024.0 / seconds, total_io_us / 1000.0 / seconds, block_hits, block_misses);
    total_io_us = total_bytes = 0;
    block_hits = block_misses = 0;
}

void obb_benchmark(void) {
    static const int64_t offsets_mb[] = {0, 64, 128, 256, 512, 1024, 1536, 1900};
    SceUID uid = sceIoOpen(OBB_PATH, SCE_O_RDONLY, 0);
    if (uid < 0) return;
    uint8_t * buf = malloc(256 * 1024);
    if (!buf) { sceIoClose(uid); return; }

    l_info("OBB benchmark: 256 KB sequential + 2x 16 KB at each offset");
    for (unsigned i = 0; i < sizeof(offsets_mb) / sizeof(offsets_mb[0]); i++) {
        int64_t off = offsets_mb[i] * 1024 * 1024;
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        int r = sceIoPread(uid, buf, 256 * 1024, off);
        SceUInt64 t1 = sceKernelGetProcessTimeWide();
        sceIoPread(uid, buf, 16 * 1024, off + 3 * 1024 * 1024);
        sceIoPread(uid, buf, 16 * 1024, off + 7 * 1024 * 1024);
        SceUInt64 t2 = sceKernelGetProcessTimeWide();
        l_info("  %5lld MB: seq %.1f MB/s (%d), 16 KB random %.1f ms each", offsets_mb[i],
               r > 0 ? r / (double) (t1 - t0) : 0.0, r, (t2 - t1) / 2000.0);
    }
    free(buf);
    sceIoClose(uid);
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
