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

#include "xylem.h"
#include "platform/platform.h"

#define BUFSIZE 4096

typedef struct _logger_s      _logger_t;
typedef struct _printer_ctx_s _printer_ctx_t;

struct _printer_ctx_s {
    xylem_logger_level_t level;
    char                 message[];
};

struct _logger_s {
    bool                 async;
    xylem_logger_level_t level;
    FILE*                file;
    const char*          filename;
    size_t               max_file_size;
    mtx_t                mtx;
    xylem_thrdpool_t*    thrdpool;
    atomic_bool          initialized;
    once_flag            once;
    void (*callback)(xylem_logger_level_t level, const char* restrict msg);
};

static _logger_t   _logger = { .once = ONCE_FLAG_INIT };
static const char* _levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};

/* Init params passed through file-scope vars for call_once. */
static const char*           _init_filename;
static xylem_logger_level_t  _init_level;
static bool                  _init_async;
static size_t                _init_max_file_size;

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
    }
}

/* Print a formatted log message to file or callback. */
static void _logger_print_message(void* param) {
    _printer_ctx_t* ctx = param;

    mtx_lock(&_logger.mtx);
    if (_logger.callback) {
        _logger.callback(ctx->level, ctx->message);
    } else {
        _logger_check_rollover();
        fprintf(_logger.file, "%s", ctx->message);
        fflush(_logger.file);
    }
    mtx_unlock(&_logger.mtx);
    free(ctx);
}

/* Build a formatted log line into buf. Returns length excluding NUL. */
static int _logger_build_message(
    char*                buf,
    size_t               buflen,
    xylem_logger_level_t level,
    bool                 has_callback,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    va_list              v) {
    int       off = 0;
    struct tm tm;

    platform_tid_t tid = platform_info_gettid();

    if (has_callback) {
        /* callback mode: just tid file:line prefix */
        off = snprintf(buf, buflen, "%lu %s:%d ", (unsigned long)tid, file, line);
    } else {
        struct timespec tsc;
        (void)timespec_get(&tsc, TIME_UTC);
        platform_info_getlocaltime(&tsc.tv_sec, &tm);

        off = snprintf(
            buf,
            buflen,
            "%04d-%02d-%02d %02d:%02d:%02d.%03d %lu %5s %s:%d ",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            (int)(tsc.tv_nsec / 1000000UL),
            (unsigned long)tid,
            _levels[level],
            file,
            line);
    }
    if (off < 0 || (size_t)off >= buflen) {
        off = (int)buflen - 1;
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
    return off;
}

static void _sync_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    va_list              v) {
    char buf[BUFSIZE];

    mtx_lock(&_logger.mtx);
    bool has_callback = _logger.callback != NULL;
    _logger_build_message(buf, sizeof(buf), level, has_callback, file, line, fmt, v);

    if (_logger.callback) {
        _logger.callback(level, buf);
    } else {
        _logger_check_rollover();
        fprintf(_logger.file, "%s", buf);
        fflush(_logger.file);
    }
    mtx_unlock(&_logger.mtx);
}

static void _async_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    va_list              v) {
    char buf[BUFSIZE];

    mtx_lock(&_logger.mtx);
    bool has_callback = _logger.callback != NULL;
    mtx_unlock(&_logger.mtx);

    int len = _logger_build_message(buf, sizeof(buf), level, has_callback, file, line, fmt, v);

    _printer_ctx_t* ctx = malloc(sizeof(_printer_ctx_t) + len + 1);
    if (ctx) {
        memcpy(ctx->message, buf, len + 1);
        ctx->level = level;
        xylem_thrdpool_post(_logger.thrdpool, _logger_print_message, ctx);
    }
}

/* call_once callback for initialization. */
static void _logger_do_init(void) {
    if (_init_level > XYLEM_LOGGER_LEVEL_ERROR) {
        _logger.level = XYLEM_LOGGER_LEVEL_INFO;
    } else {
        _logger.level = _init_level;
    }
    if (_init_filename) {
        _logger.file     = platform_io_fopen(_init_filename, "a+");
        _logger.filename = _init_filename;
    } else {
        _logger.file     = stdout;
        _logger.filename = NULL;
    }
    _logger.max_file_size = _init_max_file_size;
    if (_init_async) {
        _logger.thrdpool = xylem_thrdpool_create(1);
        _logger.async    = true;
    } else {
        _logger.async = false;
    }
    _logger.callback = NULL;
    mtx_init(&_logger.mtx, mtx_plain);
    atomic_store(&_logger.initialized, true);
}

void xylem_logger_init(
    const char* restrict filename,
    xylem_logger_level_t level,
    bool                 async,
    size_t               max_file_size) {
    _init_filename      = filename;
    _init_level         = level;
    _init_async         = async;
    _init_max_file_size = max_file_size;
    call_once(&_logger.once, _logger_do_init);
}

void xylem_logger_destroy(void) {
    bool expected = true;
    if (atomic_compare_exchange_strong(&_logger.initialized, &expected, false)) {
        if (_logger.async) {
            xylem_thrdpool_destroy(_logger.thrdpool);
            _logger.thrdpool = NULL;
        }
        if (_logger.file != stdout && _logger.file != NULL) {
            fclose(_logger.file);
        }
        _logger.file          = NULL;
        _logger.filename      = NULL;
        _logger.max_file_size = 0;
        _logger.callback      = NULL;
        _logger.async         = false;
        mtx_destroy(&_logger.mtx);

        /* reset once_flag so init can be called again */
        _logger.once = (once_flag)ONCE_FLAG_INIT;
    }
}

void xylem_logger_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int                  line,
    const char* restrict fmt,
    ...) {
    if (!atomic_load(&_logger.initialized)) {
        return;
    }
    if (_logger.level > level) {
        return;
    }
    /* strip directory prefix from file path */
    const char* p = strrchr(file, PLATFORM_PATH_SEPARATOR);

    va_list v;
    va_start(v, fmt);
    if (_logger.async) {
        _async_log(level, p ? p + 1 : file, line, fmt, v);
    } else {
        _sync_log(level, p ? p + 1 : file, line, fmt, v);
    }
    va_end(v);
}

void xylem_logger_set_callback(
    void (*callback)(xylem_logger_level_t level, const char* restrict msg)) {
    mtx_lock(&_logger.mtx);
    _logger.callback = callback;
    mtx_unlock(&_logger.mtx);
}
