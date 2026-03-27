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

#include <stdbool.h>
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

/**
 * @brief Initialize the logger.
 *
 * @param filename       Log file path, or NULL for stdout.
 * @param level          Minimum log level to output.
 * @param async          If true, logs are written asynchronously via thread pool.
 * @param max_file_size  Maximum log file size in bytes before rollover (truncate
 *                       and restart from beginning). 0 means no limit.
 *                       Ignored when filename is NULL (stdout).
 */
extern void xylem_logger_init(
    const char* restrict filename,
    xylem_logger_level_t level,
    bool                 async,
    size_t               max_file_size);

/**
 * @brief Deinitialize the logger and release resources.
 */
extern void xylem_logger_deinit(void);

/**
 * @brief Set a custom callback for log output.
 *
 * @param callback  Function to receive log messages. If set, file output is bypassed.
 */
extern void xylem_logger_set_callback(void (*callback)(xylem_logger_level_t level, const char* restrict msg));

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
