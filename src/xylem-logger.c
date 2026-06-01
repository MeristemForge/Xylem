/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "xylem/xylem-logger.h"
#include "thrdpool.h"

#include "thrds.h"

#include "platform/platform.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFSIZE 4096

typedef struct _logger_s      _logger_t;
typedef struct _printer_ctx_s _printer_ctx_t;

struct _printer_ctx_s {
    xylem_logger_level_t level;
    int                  cb_offset; /* offset to callback-format substring */
    char                 message[];
};

enum {
    LOGGER_UNINIT = 0,
    LOGGER_INIT   = 1,
    LOGGER_INITED = 2,
};

struct _logger_s {
    bool                 async;
    xylem_logger_level_t level;
    FILE*                file;
    const char*          filename;
    size_t               max_file_size;
    mtx_t                mtx;
    thrdpool_t*          thrdpool;
    atomic_int           state;
    atomic_int           inflight; /* log calls currently touching resources */
    void (*callback)(xylem_logger_level_t level, const char* restrict msg,
                     void* ud);
    void*                callback_ud;
};

static _logger_t   _logger;
static const char* _levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};

/**
 * Take an in-flight reference and confirm the logger is live. deinit flips
 * state to UNINIT and then waits for the in-flight count to drain to zero, so
 * a reference acquired here keeps the thrdpool, mtx and file alive until the
 * matching _logger_unref. Returns false (and holds no reference) when the
 * logger is not initialized.
 */
static bool _logger_ref(void) {
    atomic_fetch_add(&_logger.inflight, 1);
    if (atomic_load(&_logger.state) != LOGGER_INITED) {
        atomic_fetch_sub(&_logger.inflight, 1);
        return false;
    }
    return true;
}

/* Release a reference taken by _logger_ref. */
static void _logger_unref(void) {
    atomic_fetch_sub(&_logger.inflight, 1);
}

/* Check file size and rollover (truncate) if over threshold. Caller holds mtx. */
static void _logger_check_rollover(void) {
    if (_logger.max_file_size == 0 || _logger.file == NULL ||
        _logger.file == stdout || _logger.filename == NULL) {
        return;
    }
    long pos = ftell(_logger.file);
    if (pos >= 0 && (size_t)pos > _logger.max_file_size) {
        fclose(_logger.file);
        _logger.file = platform_io_fopen(_logger.filename, "w");
        /**
         * Fall back to stdout so logging survives a failed reopen instead of
         * dereferencing NULL on the next write.
         */
        if (!_logger.file) {
            free((void*)_logger.filename);
            _logger.file     = stdout;
            _logger.filename = NULL;
        }
    }
}

/* Print a formatted log message to file or callback. */
static void _logger_print_message(void* param) {
    _printer_ctx_t* ctx = param;

    mtx_lock(&_logger.mtx);
    if (_logger.callback) {
        _logger.callback(ctx->level, ctx->message + ctx->cb_offset,
                         _logger.callback_ud);
    } else {
        _logger_check_rollover();
        fprintf(_logger.file, "%s", ctx->message);
        fflush(_logger.file);
    }
    mtx_unlock(&_logger.mtx);
    free(ctx);
}

/**
 * Build a formatted log line into buf. Returns length excluding NUL.
 * Always produces the full format (timestamp + tid + level + file:line + msg).
 * *out_cb_offset receives the offset where the callback-format substring
 * (tid + file:line + msg) begins, so the caller can choose which view to use
 * without reformatting.
 */
static int _logger_build_message(
    char*                buf,
    size_t               buflen,
    xylem_logger_level_t level,
    int*                 out_cb_offset,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    va_list              v) {
    int off = 0;

    platform_tid_t tid = platform_info_gettid();

    struct timespec tsc;
    struct tm       tm;
    (void)timespec_get(&tsc, TIME_UTC);
    platform_info_getlocaltime(&tsc.tv_sec, &tm);

    /* full format: "YYYY-MM-DD HH:MM:SS.mmm TID LEVEL file:line " */
    off = snprintf(
        buf,
        buflen,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d %" PRIu64 " %5s ",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        (int)(tsc.tv_nsec / 1000000UL),
        (uint64_t)tid,
        _levels[level]);
    if (off < 0 || (size_t)off >= buflen) {
        off = (int)buflen - 1;
    }

    /* callback-format substring starts here: "file:line msg\n" */
    int cb_off = off;

    int n = snprintf(
        buf + off,
        buflen - off,
        "%s:%d ",
        file,
        line);
    if (n < 0 || (size_t)(off + n) >= buflen) {
        off = (int)buflen - 1;
    } else {
        off += n;
    }

    int written = platform_io_vsprintf(buf + off, buflen - off, fmt, v);
    if (written < 0) {
        written = 0;
    }
    off += written;
    if ((size_t)off >= buflen - 1) {
        off = (int)buflen - 2;
    }
    buf[off++] = '\n';
    buf[off]   = '\0';

    *out_cb_offset = cb_off;
    return off;
}

static void _logger_sync_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    va_list              v) {
    char buf[BUFSIZE];
    int  cb_offset;

    _logger_build_message(buf, sizeof(buf), level, &cb_offset, file, line, fmt, v);

    mtx_lock(&_logger.mtx);
    if (_logger.callback) {
        _logger.callback(level, buf + cb_offset, _logger.callback_ud);
    } else {
        _logger_check_rollover();
        fprintf(_logger.file, "%s", buf);
        fflush(_logger.file);
    }
    mtx_unlock(&_logger.mtx);
}

static void _logger_async_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    va_list              v) {
    char buf[BUFSIZE];
    int  cb_offset;

    int len = _logger_build_message(buf, sizeof(buf), level, &cb_offset, file, line, fmt, v);

    _printer_ctx_t* ctx = (_printer_ctx_t*)malloc(sizeof(_printer_ctx_t) + len + 1);
    if (!ctx) {
        return;
    }
    memcpy(ctx->message, buf, len + 1);
    ctx->level     = level;
    ctx->cb_offset = cb_offset;
    /* On submit failure the pool never takes ownership, so free ctx here. */
    if (thrdpool_submit(_logger.thrdpool, _logger_print_message, ctx) != 0) {
        free(ctx);
    }
}

/* Initialize logger state. Called by the single thread that wins the CAS. */
static void _logger_do_init(
    const char*          filename,
    xylem_logger_level_t level,
    bool                 async,
    size_t               max_file_size) {
    if (level > XYLEM_LOGGER_LEVEL_ERROR) {
        _logger.level = XYLEM_LOGGER_LEVEL_INFO;
    } else {
        _logger.level = level;
    }
    if (filename) {
        _logger.file = platform_io_fopen(filename, "a+");
        /**
         * Own a private copy of the path: rollover reopens the file later, so
         * the logger cannot rely on the caller keeping filename alive.
         */
        if (_logger.file) {
            size_t n    = strlen(filename) + 1;
            char*  copy = (char*)malloc(n);
            if (copy) {
                memcpy(copy, filename, n);
            }
            _logger.filename = copy;
        }
        /* Fall back to stdout so a failed open never leaves file NULL. */
        if (!_logger.file) {
            _logger.file     = stdout;
            _logger.filename = NULL;
        }
    } else {
        _logger.file     = stdout;
        _logger.filename = NULL;
    }
    _logger.max_file_size = max_file_size;
    if (async) {
        _logger.thrdpool = thrdpool_create(1);
    }
    /**
     * Fall back to synchronous logging when the worker pool fails to start,
     * otherwise log() would submit to a NULL pool and crash.
     */
    _logger.async = (async && _logger.thrdpool != NULL);
    _logger.callback = NULL;
    mtx_init(&_logger.mtx, mtx_plain);
    atomic_store(&_logger.state, LOGGER_INITED);
}

void xylem_logger_init(
    const char* restrict       filename,
    const xylem_logger_opts_t* opts) {
    int expected = LOGGER_UNINIT;
    if (atomic_compare_exchange_strong(&_logger.state, &expected, LOGGER_INIT)) {
        /**
         * Defaults match the documented behaviour when opts is NULL:
         * INFO threshold, no rollover.
         */
        xylem_logger_level_t level         = XYLEM_LOGGER_LEVEL_INFO;
        size_t               max_file_size = 0;
        if (opts) {
            level         = opts->level;
            max_file_size = opts->max_file_size;
        }
        /**
         * The public API only exposes the async path. The internal
         * _logger_do_init still takes an async flag so the sync path
         * below remains exercisable from in-tree debugging code.
         */
        _logger_do_init(filename, level, true, max_file_size);
    }
}

void xylem_logger_deinit(void) {
    int expected = LOGGER_INITED;
    if (atomic_compare_exchange_strong(&_logger.state, &expected, LOGGER_UNINIT)) {
        /**
         * State is now UNINIT, so _logger_ref now fails for new callers. Wait
         * for outstanding references to drain before freeing resources.
         */
        while (atomic_load(&_logger.inflight) != 0) {
            thrd_yield();
        }
        if (_logger.async) {
            thrdpool_destroy(_logger.thrdpool);
            _logger.thrdpool = NULL;
        }
        if (_logger.file != stdout && _logger.file != NULL) {
            fclose(_logger.file);
        }
        free((void*)_logger.filename);
        _logger.file          = NULL;
        _logger.filename      = NULL;
        _logger.max_file_size = 0;
        _logger.callback      = NULL;
        _logger.callback_ud   = NULL;
        _logger.async         = false;
        mtx_destroy(&_logger.mtx);
    }
}

void xylem_logger_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    ...) {
    if (!_logger_ref()) {
        return;
    }
    /* Guard the _levels[] index against an out-of-range level argument. */
    if (level > XYLEM_LOGGER_LEVEL_ERROR || _logger.level > level) {
        _logger_unref();
        return;
    }
    /* strip directory prefix from file path */
    const char* p = strrchr(file, PLATFORM_PATH_SEPARATOR);

    va_list v;
    va_start(v, fmt);
    if (_logger.async) {
        _logger_async_log(level, p ? p + 1 : file, line, fmt, v);
    } else {
        _logger_sync_log(level, p ? p + 1 : file, line, fmt, v);
    }
    va_end(v);

    _logger_unref();
}

void xylem_logger_set_callback(
    void (*callback)(xylem_logger_level_t level,
                     const char* restrict msg,
                     void* ud),
    void* ud) {
    /* mtx is only valid while a reference is held against a concurrent deinit. */
    if (!_logger_ref()) {
        return;
    }
    mtx_lock(&_logger.mtx);
    _logger.callback    = callback;
    _logger.callback_ud = ud;
    mtx_unlock(&_logger.mtx);
    _logger_unref();
}
