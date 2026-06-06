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

_Pragma("once")

#include <stddef.h>

#define xylem_logd(...)    xylem_logger_log(XYLEM_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define xylem_logi(...)    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define xylem_logw(...)    xylem_logger_log(XYLEM_LOGGER_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define xylem_loge(...)    xylem_logger_log(XYLEM_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

typedef enum xylem_logger_level_e xylem_logger_level_t;

enum xylem_logger_level_e {
    XYLEM_LOGGER_LEVEL_DEBUG,
    XYLEM_LOGGER_LEVEL_INFO,
    XYLEM_LOGGER_LEVEL_WARN,
    XYLEM_LOGGER_LEVEL_ERROR,
};

typedef struct xylem_logger_opts_s xylem_logger_opts_t;

/**
 * Logger configuration options.
 *
 * Pass NULL to xylem_logger_init() to use defaults (INFO level,
 * no file-size limit). When opts is non-NULL every field is taken
 * verbatim; there is no "zero means default" rule here because the
 * level enum starts at DEBUG = 0. Initialise the fields you care
 * about explicitly, for example:
 *
 *     xylem_logger_opts_t opts = {
 *         .level         = XYLEM_LOGGER_LEVEL_INFO,
 *         .max_file_size = 10 * 1024 * 1024,
 *     };
 *     xylem_logger_init("app.log", &opts);
 */
struct xylem_logger_opts_s {
    xylem_logger_level_t level;         /* Minimum log level to output. */
    /**
     * Rollover threshold in bytes before the log file is truncated and
     * restarted. 0 means no limit. Ignored when filename is NULL (stdout).
     */
    size_t               max_file_size;
};

/**
 * @brief Initialize the logger.
 *
 * Log writes are dispatched asynchronously on a dedicated worker
 * thread so the calling thread is never blocked by file I/O or by
 * the user-supplied callback.
 *
 * @param filename  Log file path, or NULL for stdout.
 * @param opts      Logger options, or NULL for defaults
 *                  (level = INFO, max_file_size = 0).
 */
extern void xylem_logger_init(
    const char* restrict       filename,
    const xylem_logger_opts_t* opts);

/**
 * @brief Deinitialize the logger and release resources.
 */
extern void xylem_logger_deinit(void);

/**
 * @brief Set a custom callback for log output.
 *
 * @param callback  Function to receive log messages. If set, file output is bypassed.
 * @param ud        Opaque pointer passed to the callback on each invocation.
 */
extern void xylem_logger_set_callback(
    void (*callback)(xylem_logger_level_t level,
                     const char* restrict msg,
                     void* ud),
    void* ud);

/**
 * @brief Log a message (internal, use xylem_logd/i/w/e macros instead).
 *
 * @param level  Log level.
 * @param file   Source file name.
 * @param line   Source line number.
 * @param fmt    printf-style format string.
 * @param ...    Format arguments.
 */
extern void xylem_logger_log(xylem_logger_level_t level, const char* restrict file, int line, const char* restrict fmt, ...);
