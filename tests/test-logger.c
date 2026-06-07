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

/**
 * The logger always dispatches to its internal worker thread, so the
 * callback fires from a different thread than the one that called
 * xylem_logger_log. Tests take advantage of the fact that
 * xylem_logger_deinit() drains the queue (thrdpool_destroy joins the
 * worker), which establishes a happens-before edge and makes every
 * callback-side write visible to the reader.
 *
 * Pattern: log everything for the case, call deinit, then assert.
 * The ctx records the level and message of every callback invocation
 * so multi-event cases can still verify the full sequence.
 */

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

static void _reset_ctx(_logger_ctx_t* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* init/deinit without logging. */
static void test_init_destroy(void) {
    xylem_logger_init(NULL, NULL);
    xylem_logger_deinit();
}

/* log before init should not crash. */
static void test_log_before_init(void) {
    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 1, "should be ignored");
}

/* callback receives correct level and message content. */
static void test_callback_receives_message(void) {
    _logger_ctx_t ctx;
    _reset_ctx(&ctx);
    xylem_logger_opts_t opts = { .level = XYLEM_LOGGER_LEVEL_DEBUG };
    xylem_logger_init(NULL, &opts);
    xylem_logger_set_callback(_test_callback, &ctx);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", 42, "hello %d", 123);

    xylem_logger_deinit();

    ASSERT(ctx.count == 1);
    ASSERT(ctx.levels[0] == XYLEM_LOGGER_LEVEL_INFO);
    ASSERT(strstr(ctx.last_msg, "hello 123") != NULL);
    ASSERT(strstr(ctx.last_msg, "test.c:42") != NULL);
}

/* level filtering: messages below threshold are suppressed. */
static void test_level_filtering(void) {
    _logger_ctx_t ctx;
    _reset_ctx(&ctx);
    xylem_logger_opts_t opts = { .level = XYLEM_LOGGER_LEVEL_WARN };
    xylem_logger_init(NULL, &opts);
    xylem_logger_set_callback(_test_callback, &ctx);

    xylem_logger_log(XYLEM_LOGGER_LEVEL_DEBUG, "test.c", 1, "debug");
    xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO,  "test.c", 2, "info");
    xylem_logger_log(XYLEM_LOGGER_LEVEL_WARN,  "test.c", 3, "warn");
    xylem_logger_log(XYLEM_LOGGER_LEVEL_ERROR, "test.c", 4, "error");

    xylem_logger_deinit();

    /* DEBUG and INFO are below the WARN threshold and must not fire. */
    ASSERT(ctx.count == 2);
    ASSERT(ctx.levels[0] == XYLEM_LOGGER_LEVEL_WARN);
    ASSERT(ctx.levels[1] == XYLEM_LOGGER_LEVEL_ERROR);
}

/* log macros produce correct levels. */
static void test_log_macros(void) {
    _logger_ctx_t ctx;
    _reset_ctx(&ctx);
    xylem_logger_opts_t opts = { .level = XYLEM_LOGGER_LEVEL_DEBUG };
    xylem_logger_init(NULL, &opts);
    xylem_logger_set_callback(_test_callback, &ctx);

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

/* file output: write to file and verify content. */
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

/* file rollover: file is truncated when exceeding max_file_size. */
static void test_file_rollover(void) {
    remove(LOG_FILE);

    /* set a small threshold so a few log lines will exceed it */
    size_t max_size = 200;
    xylem_logger_opts_t opts = {
        .level         = XYLEM_LOGGER_LEVEL_DEBUG,
        .max_file_size = max_size,
    };
    xylem_logger_init(LOG_FILE, &opts);

    /* write enough lines to exceed the threshold */
    for (int32_t i = 0; i < 10; i++) {
        xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO, "test.c", i, "rollover line %d padding padding padding", i);
    }
    xylem_logger_deinit();

    /* after rollover the file should be smaller than max_size + one log line */
    FILE* f = fopen(LOG_FILE, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    /* file must have been truncated -- should be well under 2x threshold */
    ASSERT(size < (long)(max_size * 2));

    /* file should contain the last written line, not the first */
    f = fopen(LOG_FILE, "r");
    ASSERT(f != NULL);
    char buf[4096] = {0};
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    ASSERT(rd > 0);
    buf[rd] = '\0';
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
    return 0;
}
