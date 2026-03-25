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

/**
 * HTTP Session Client
 *
 * Demonstrates session-based connection pooling. Sends multiple
 * requests to the same server, reusing the underlying TCP connection
 * via HTTP keep-alive.
 *
 * Usage: http-session-client
 * Pair:  http-echo-server
 */

#include "xylem.h"
#include <stdio.h>

#define BASE_URL "http://127.0.0.1:8080"

static void _print_res(const char* label, xylem_http_res_t* res) {
    if (!res) {
        printf("%s: request failed\n", label);
        return;
    }
    printf("%s: %d  body(%zu): %.*s\n", label,
           xylem_http_res_status(res),
           xylem_http_res_body_len(res),
           (int)xylem_http_res_body_len(res),
           (const char*)xylem_http_res_body(res));
    xylem_http_res_destroy(res);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    /* Create a session with default options. */
    xylem_http_session_t* session = xylem_http_session_create(NULL);
    if (!session) {
        printf("failed to create session\n");
        return 1;
    }

    /* First GET -- establishes a new connection. */
    xylem_http_res_t* res = xylem_http_session_get(session,
                                                    BASE_URL "/", NULL);
    _print_res("GET / (1st)", res);

    /* Second GET -- reuses the pooled connection. */
    res = xylem_http_session_get(session, BASE_URL "/", NULL);
    _print_res("GET / (2nd)", res);

    /* POST -- also reuses the pooled connection. */
    const char* body = "hello from session";
    res = xylem_http_session_post(session, BASE_URL "/echo",
                                  body, strlen(body),
                                  "text/plain", NULL);
    _print_res("POST /echo", res);

    /* Destroy session -- closes all pooled connections. */
    xylem_http_session_destroy(session);

    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
