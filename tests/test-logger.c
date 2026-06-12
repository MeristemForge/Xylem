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

#define LOG_FILE   "test-logger-output.log"
#define MAX_EVENTS 8

typedef struct {
    xylem_logger_level_t levels[MAX_EVENTS];
    char                 last_msg[4096];
    int                  count;
} _logger_ctx_t;

static void _test_callback(xylem_logger_level_t level,
                           const char* restrict msg,
                           void* ud) {
    _logger_ctx_t* ctx = (_logger_ctx_t*)ud;
    if (ctx->count < MAX_EVENTS) {
        ctx->levels[ctx->count] = level;
    }
    strncpy(ctx->last_msg, msg, sizeof(ctx->last_msg) - 1);
    ctx->last_msg[sizeof(ctx->last_msg) - 1] = '\0';
    ctx->count++;
}

static void _capture_start(_logger_ctx_t* ctx, xylem_logger_level_t level) {
    memset(ctx, 0, sizeof(*ctx));
    xylem_logger_opts_t opts = { .level = level };
    xylem_logger_init(NULL, &opts);
    xylem_logger_set_callback(_test_callback, ctx);
}

static void test_init_destroy(void) {
    xylem_logger_init(NULL, NULL);
    xylem_logger_deinit();
}

static void test_log_before_init(void) {
    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 1, "should be ignored");
}

static void test_callback_receives_message(void) {
    _logger_ctx_t ctx;
    _capture_start(&ctx, XYLEM_LOGGER_LEVEL_DEBUG);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 42, "hello %d", 123);

    xylem_logger_deinit();

    ASSERT(ctx.count == 1);
    ASSERT(ctx.levels[0] == XYLEM_LOGGER_LEVEL_INFO);
    ASSERT(strstr(ctx.last_msg, "hello 123") != NULL);
    ASSERT(strstr(ctx.last_msg, "test.c:42") != NULL);
}

static void test_level_filtering(void) {
    _logger_ctx_t ctx;
    _capture_start(&ctx, XYLEM_LOGGER_LEVEL_WARN);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_DEBUG, "test.c", 1, "debug");
    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO,  "test.c", 2, "info");
    xylem_logger_log(XYLEM_LOGGER_LEVEL_WARN,  "test.c", 3, "warn");
    xylem_logger_log(XYLEM_LOGGER_LEVEL_ERROR, "test.c", 4, "error");

    xylem_logger_deinit();

    ASSERT(ctx.count == 2);
    ASSERT(ctx.levels[0] == XYLEM_LOGGER_LEVEL_WARN);
    ASSERT(ctx.levels[1] == XYLEM_LOGGER_LEVEL_ERROR);
}

static void test_log_macros(void) {
    _logger_ctx_t ctx;
    _capture_start(&ctx, XYLEM_LOGGER_LEVEL_DEBUG);

    xylem_logd("debug msg");
    xylem_logi("info msg");
    xylem_logw("warn msg");
    xylem_loge("error msg");

    xylem_logger_deinit();

    ASSERT(ctx.count == 4);
    ASSERT(ctx.levels[0] == XYLEM_LOGGER_LEVEL_DEBUG);
    ASSERT(ctx.levels[1] == XYLEM_LOGGER_LEVEL_INFO);
    ASSERT(ctx.levels[2] == XYLEM_LOGGER_LEVEL_WARN);
    ASSERT(ctx.levels[3] == XYLEM_LOGGER_LEVEL_ERROR);
}

static void test_file_output(void) {
    remove(LOG_FILE);
    xylem_logger_opts_t opts = { .level = XYLEM_LOGGER_LEVEL_DEBUG };
    xylem_logger_init(LOG_FILE, &opts);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 99, "file test %s", "ok");
    xylem_logger_deinit();

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

static void test_file_rollover(void) {
    remove(LOG_FILE);

    size_t max_size = 200;
    xylem_logger_opts_t opts = {
        .level         = XYLEM_LOGGER_LEVEL_DEBUG,
        .max_file_size = max_size,
    };
    xylem_logger_init(LOG_FILE, &opts);

    for (int32_t i = 0; i < 10; i++) {
        xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", i,
                         "rollover line %d padding padding padding", i);
    }
    xylem_logger_deinit();

    FILE* f = fopen(LOG_FILE, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    ASSERT(size < (long)(max_size * 2));

    f = fopen(LOG_FILE, "r");
    ASSERT(f != NULL);
    char buf[4096] = {0};
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    ASSERT(rd > 0);
    buf[rd] = '\0';
    fclose(f);
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
    return 0;
}
