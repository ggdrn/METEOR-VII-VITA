/*
 * Force-included (-include) into third-party sources that print with
 * sceClibPrintf directly (so_util, FalsoJNI), so their output also reaches
 * logs.txt through the loader's logger.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_LOG_REDIRECT_H
#define SOLOADER_LOG_REDIRECT_H

int soloader_printf(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

#define sceClibPrintf soloader_printf

#endif // SOLOADER_LOG_REDIRECT_H
