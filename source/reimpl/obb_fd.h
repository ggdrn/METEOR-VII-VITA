/*
 * Buffered file descriptors over the original OBB.
 *
 * The FF7 engine reads its .lgp archives in small chunks; on the Vita every
 * read() is a syscall to the memory card, which made the first load take
 * ~40 s. ExpansionFile hands out these descriptors instead of raw ones, and
 * read/lseek/lseek64/close route them here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_OBB_FD_H
#define SOLOADER_OBB_FD_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// Far above newlib's descriptor range, so they can't collide
#define OBB_FD_BASE 0x4000
#define OBB_FD_MAX  32

/** Opens a new descriptor positioned at 0 of the OBB file. Returns -1 on error. */
int obb_fd_open(const char * tag);

bool obb_fd_is(int fd);

ssize_t obb_fd_read(int fd, void * buf, size_t count);

int64_t obb_fd_lseek(int fd, int64_t offset, int whence);

int obb_fd_close(int fd);

/** Measures card read speed at several OBB offsets (diagnostic). */
void obb_benchmark(void);

/** Whether [off, off+len) of the OBB is in the RAM cache of whole-file reads. */
bool obb_cache_has(int64_t off, size_t len);

/**
 * Reads [off, off+len) of the OBB into that cache (any thread), so a later
 * read of the whole entry by the game is served from RAM. Returns the bytes
 * read, or < 0 if it wasn't cached (too small or large, no memory, IO error).
 */
int obb_cache_prefetch(int64_t off, size_t len, const char * tag);

/** Finds an OBB entry the way ExpansionFile does (java.c). */
bool obb_entry_find(const char * path, int64_t * off, int64_t * len);

/** Logs OBB read totals accumulated over `seconds`, then resets them. */
void obb_fd_report(double seconds);

#endif // SOLOADER_OBB_FD_H
