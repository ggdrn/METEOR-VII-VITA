/*
 * In-memory scratch files (see memfile.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_MEMFILE_H
#define SOLOADER_MEMFILE_H

#include <stdint.h>
#include <sys/types.h>

/** True for the paths kept in RAM. */
int memfile_handles(const char * path);

/** True for descriptors returned by memfile_open. */
int memfile_is(int fd);

/** Opens `path` (bionic O_* flags). -1 with errno on error, -2 if the RAM
 *  tables are full (the caller should fall back to the card). */
int memfile_open(const char * path, int bionic_flags);
int memfile_close(int fd);
ssize_t memfile_read(int fd, void * buf, size_t count);
ssize_t memfile_write(int fd, const void * buf, size_t count);
int64_t memfile_lseek(int fd, int64_t offset, int whence);
uint32_t memfile_size(int fd);

#endif // SOLOADER_MEMFILE_H
