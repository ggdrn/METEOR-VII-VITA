/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "utils/cpu.h"

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Formatted message.
static char buffer_b[2048];
// Plain-text line written to the log file.
static char buffer_file[2304];

#ifdef ENABLE_LOGGING
static SceUID _log_fd = -1;
static bool _log_file_tried = false;

// Lines are collected in RAM and written by a low-priority thread about once
// a second: writing every line straight to the card competed with the game's
// own reads. Fatal messages flush immediately (see log_flush).
#define LOG_BUF_SIZE   (64 * 1024)
#define LOG_FLUSH_AT   (48 * 1024)
#define LOG_FLUSH_US   1000000
static char _log_buf[LOG_BUF_SIZE];
static char _log_out[LOG_BUF_SIZE];
static int _log_len;
static SceKernelLwMutexWork _flush_mutex;
static void _log_flush_locked_buffer(void);
static void _log_start_writer(void);
#endif

static void _log_lock() {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 2 /* recursive */, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
#ifdef ENABLE_LOGGING
        _log_start_writer();
#endif
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);
}

static void _log_unlock() {
    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}

// Must be called with the log mutex held.
static void _log_file_write(const char * text, int len) {
#ifdef ENABLE_LOGGING
    if (!_log_file_tried) {
        _log_file_tried = true;
        sceIoMkdir(DATA_PATH, 0777);
        // Keep the previous session around: the one that crashed is usually
        // the interesting one, and the next launch would overwrite it.
        sceIoRemove(DATA_PATH "logs_anterior.txt");
        sceIoRename(LOG_PATH, DATA_PATH "logs_anterior.txt");
        _log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (_log_fd >= 0) {
            char header[160];
            int n = sceClibSnprintf(header, sizeof(header),
                                    "FINAL FANTASY VII (PS Vita) %s - build %s %s\n"
                                    "------------------------------------------------\n",
                                    PORT_VERSION, __DATE__, __TIME__);
            sceIoWrite(_log_fd, header, n);
        }
    }
    if (_log_fd < 0 || len <= 0) return;
    if (len > LOG_BUF_SIZE - _log_len) _log_flush_locked_buffer();
    if (len > LOG_BUF_SIZE) len = LOG_BUF_SIZE;
    sceClibMemcpy(_log_buf + _log_len, text, len);
    _log_len += len;
    if (_log_len >= LOG_FLUSH_AT) _log_flush_locked_buffer();
#else
    (void)text; (void)len;
#endif
}

#ifdef ENABLE_LOGGING
// Called with the log mutex held: hands the buffer to the card writer.
static void _log_flush_locked_buffer(void) {
    if (_log_len == 0 || _log_fd < 0) return;
    sceKernelLockLwMutex(&_flush_mutex, 1, NULL);
    int n = _log_len;
    sceClibMemcpy(_log_out, _log_buf, n);
    _log_len = 0;
    sceIoWrite(_log_fd, _log_out, n);
    sceKernelUnlockLwMutex(&_flush_mutex, 1);
}

static int _log_writer_thread(SceSize args, void * argp) {
    while (1) {
        sceKernelDelayThread(LOG_FLUSH_US);
        log_flush();
    }
    return 0;
}

static void _log_start_writer(void) {
    sceKernelCreateLwMutex(&_flush_mutex, "log_flush", 0, 0, NULL);
    SceUID thid = sceKernelCreateThread("log_writer", _log_writer_thread, 0x10000100 + 0x10, 0x2000, 0, CPU_MASK_SECONDARY, NULL);
    if (thid >= 0) cpu_register_thread("log", thid);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}
#endif

void log_flush(void) {
#ifdef ENABLE_LOGGING
    // Copy under the log lock, write to the card without it, so threads that
    // log meanwhile don't wait for the card
    static char out[LOG_BUF_SIZE];
    sceKernelLockLwMutex(&_flush_mutex, 1, NULL);
    _log_lock();
    int n = _log_len;
    if (n) sceClibMemcpy(out, _log_buf, n);
    _log_len = 0;
    _log_unlock();
    if (n && _log_fd >= 0) sceIoWrite(_log_fd, out, n);
    sceKernelUnlockLwMutex(&_flush_mutex, 1);
#endif
}

void _log_print(int t, const char* fmt, ...) {
    const char * label;
    const char * color;

    switch (t) {
        case LT_DEBUG:   label = "debug";   color = COLOR_PINK;   break;
        case LT_INFO:    label = "info";    color = COLOR_BLUE;   break;
        case LT_WARN:    label = "warning"; color = COLOR_ORANGE; break;
        case LT_ERROR:   label = "error";   color = COLOR_RED;    break;
        case LT_FATAL:   label = "fatal";   color = COLOR_RED;    break;
        case LT_SUCCESS: label = "success"; color = COLOR_GREEN;  break;
        case LT_WAIT:    label = "waiting"; color = COLOR_CYAN;   break;
        default: return;
    }

    _log_lock();

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), fmt, list);
    va_end(list);

    sceClibPrintf(" %s%-8s%s %s\n", color, label, COLOR_END, buffer_b);

    // [  12.345s][thread] level   message
    SceUInt64 us = sceKernelGetProcessTimeWide();
    int n = sceClibSnprintf(buffer_file, sizeof(buffer_file), "[%5u.%03us][%08x] %-8s %s\n",
                            (unsigned)(us / 1000000), (unsigned)((us / 1000) % 1000),
                            sceKernelGetThreadId(), label, buffer_b);
    if (n > (int)sizeof(buffer_file) - 1) n = sizeof(buffer_file) - 1;
    _log_file_write(buffer_file, n);
#ifdef ENABLE_LOGGING
    if (t == LT_FATAL) _log_flush_locked_buffer(); // the process may be about to die
#endif

    _log_unlock();
}

int soloader_printf(const char *fmt, ...) {
    _log_lock();

    va_list list;
    va_start(list, fmt);
    int n = sceClibVsnprintf(buffer_b, sizeof(buffer_b), fmt, list);
    va_end(list);
    if (n > (int)sizeof(buffer_b) - 1) n = sizeof(buffer_b) - 1;

    // Not through the macro: this file isn't force-included with log_redirect.h
    sceClibPrintf("%s", buffer_b);
    _log_file_write(buffer_b, n);

    _log_unlock();
    return n;
}
