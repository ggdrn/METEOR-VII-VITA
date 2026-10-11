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

/** Logs OBB read totals accumulated over `seconds`, then resets them. */
void obb_fd_report(double seconds);

#endif // SOLOADER_OBB_FD_H
