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
#include "assert.h"
#include <string.h>

#define LOG_FILE "test-logger-output.log"

static xylem_logger_level_t _cb_level;
static char                 _cb_msg[4096];
static int                  _cb_count;

static void _test_callback(xylem_logger_level_t level, const char* restrict msg) {
    _cb_level = level;
    strncpy(_cb_msg, msg, sizeof(_cb_msg) - 1);
    _cb_msg[sizeof(_cb_msg) - 1] = '\0';
    _cb_count++;
}

static void _reset_callback_state(void) {
    _cb_level = XYLEM_LOGGER_LEVEL_DEBUG;
    _cb_msg[0] = '\0';
    _cb_count = 0;
}

/* init/destroy without logging. */
static void test_init_destroy(void) {
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);
    xylem_logger_destroy();
}

/* log before init should not crash. */
static void test_log_before_init(void) {
    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 1, "should be ignored");
}

/* callback receives correct level and message content. */
static void test_callback_receives_message(void) {
    _reset_callback_state();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_DEBUG, false, 0);
    xylem_logger_set_callback(_test_callback);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 42, "hello %d", 123);

    ASSERT(_cb_count == 1);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_INFO);
    ASSERT(strstr(_cb_msg, "hello 123") != NULL);
    ASSERT(strstr(_cb_msg, "test.c:42") != NULL);

    xylem_logger_set_callback(NULL);
    xylem_logger_destroy();
}

/* level filtering: messages below threshold are suppressed. */
static void test_level_filtering(void) {
    _reset_callback_state();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_WARN, false, 0);
    xylem_logger_set_callback(_test_callback);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_DEBUG, "test.c", 1, "debug");
    ASSERT(_cb_count == 0);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 2, "info");
    ASSERT(_cb_count == 0);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_WARN, "test.c", 3, "warn");
    ASSERT(_cb_count == 1);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_WARN);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_ERROR, "test.c", 4, "error");
    ASSERT(_cb_count == 2);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_ERROR);

    xylem_logger_set_callback(NULL);
    xylem_logger_destroy();
}

/* log macros produce correct levels. */
static void test_log_macros(void) {
    _reset_callback_state();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_DEBUG, false, 0);
    xylem_logger_set_callback(_test_callback);

    xylem_logd("debug msg");
    ASSERT(_cb_count == 1);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_DEBUG);

    xylem_logi("info msg");
    ASSERT(_cb_count == 2);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_INFO);

    xylem_logw("warn msg");
    ASSERT(_cb_count == 3);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_WARN);

    xylem_loge("error msg");
    ASSERT(_cb_count == 4);
    ASSERT(_cb_level == XYLEM_LOGGER_LEVEL_ERROR);

    xylem_logger_set_callback(NULL);
    xylem_logger_destroy();
}

/* file output: write to file and verify content. */
static void test_file_output(void) {
    remove(LOG_FILE);
    xylem_logger_init(LOG_FILE, XYLEM_LOGGER_LEVEL_DEBUG, false, 0);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 99, "file test %s", "ok");
    xylem_logger_destroy();

    FILE* f = fopen(LOG_FILE, "r");
    ASSERT(f != NULL);
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    ASSERT(n > 0);
    ASSERT(strstr(buf, "file test ok") != NULL);
    ASSERT(strstr(buf, "INFO") != NULL);
    ASSERT(strstr(buf, "test.c:99") != NULL);

    remove(LOG_FILE);
}

/* async mode: messages are delivered via thread pool. */
static void test_async_mode(void) {
    _reset_callback_state();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_DEBUG, true, 0);
    xylem_logger_set_callback(_test_callback);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 1, "async %d", 456);

    /* give thread pool time to process */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000}; /* 50ms */
    thrd_sleep(&ts, NULL);

    ASSERT(_cb_count == 1);
    ASSERT(strstr(_cb_msg, "async 456") != NULL);

    xylem_logger_set_callback(NULL);
    xylem_logger_destroy();
}

/* file rollover: file is truncated when exceeding max_file_size. */
static void test_file_rollover(void) {
    remove(LOG_FILE);

    /* set a small threshold so a few log lines will exceed it */
    size_t max_size = 200;
    xylem_logger_init(LOG_FILE, XYLEM_LOGGER_LEVEL_DEBUG, false, max_size);

    /* write enough lines to exceed the threshold */
    for (int32_t i = 0; i < 10; i++) {
        xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", i, "rollover line %d padding padding padding", i);
    }
    xylem_logger_destroy();

    /* after rollover the file should be smaller than max_size + one log line */
    FILE* f = fopen(LOG_FILE, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    /* file must have been truncated — should be well under 2x threshold */
    ASSERT(size < (long)(max_size * 2));

    /* file should contain the last written line, not the first */
    f = fopen(LOG_FILE, "r");
    ASSERT(f != NULL);
    char buf[4096] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    /* after rollover, early lines are gone; later lines should be present */
    ASSERT(strstr(buf, "rollover line 9") != NULL);

    remove(LOG_FILE);
}

int main(void) {
    test_log_before_init();
    test_init_destroy();
    test_callback_receives_message();
    test_level_filtering();
    test_log_macros();
    test_file_output();
    test_file_rollover();
    test_async_mode();
    return 0;
}
