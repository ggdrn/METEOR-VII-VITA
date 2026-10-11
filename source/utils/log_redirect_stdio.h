/*
 * Force-included into the OpenSL ES sources: their SL_LOGx macros write to
 * stderr, which goes nowhere on the Vita. Route those writes into logs.txt.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_LOG_REDIRECT_STDIO_H
#define SOLOADER_LOG_REDIRECT_STDIO_H

#include <stdio.h>

int soloader_printf(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

// Self-referencing macros are not re-expanded, so the fallbacks call the real functions
#define fprintf(stream, ...) ((stream) == stderr ? soloader_printf(__VA_ARGS__) : fprintf(stream, __VA_ARGS__))
#define fputc(c, stream) ((stream) == stderr ? soloader_printf("%c", (c)) : fputc((c), (stream)))

#endif // SOLOADER_LOG_REDIRECT_STDIO_H
