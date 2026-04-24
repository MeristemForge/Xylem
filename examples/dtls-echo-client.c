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
 * DTLS Echo Client
 *
 * Connects to 127.0.0.1:9444 over DTLS, sends "hello", prints the
 * echoed response, then disconnects. Peer verification is disabled
 * for self-signed certificate testing.
 *
 * Usage: dtls-echo-client
 * Pair:  dtls-echo-server
 */

#include "xylem.h"
#include "xylem/xylem-dtls.h"

#define SERVER_PORT 9444

static xylem_loop_t* _loop;

static void _on_connect(xylem_dtls_conn_t* dtls) {
    xylem_logi("dtls handshake complete");
    xylem_dtls_send(dtls, "hello", 5);
}

static void _on_read(xylem_dtls_conn_t* dtls, void* data, size_t len) {
    xylem_logi("echo: %.*s", (int)len, (char*)data);
    xylem_dtls_close(dtls);
}

static void _on_close(xylem_dtls_conn_t* dtls, int err, const char* errmsg) {
    (void)dtls;
    (void)err;
    (void)errmsg;
    xylem_logi("disconnected");
    xylem_loop_stop(_loop);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    _loop = xylem_loop_create();

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    if (!ctx) {
        xylem_loge("failed to create dtls context");
        return 1;
    }

    /* Disable verification for self-signed cert testing. */
    xylem_dtls_ctx_set_verify(ctx, false);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", SERVER_PORT, &addr);

    xylem_dtls_handler_t handler = {
        .on_connect = _on_connect,
        .on_read    = _on_read,
        .on_close   = _on_close,
    };

    xylem_dtls_conn_t* dtls = xylem_dtls_dial(_loop, &addr, ctx, &handler);
    if (!dtls) {
        xylem_loge("failed to connect");
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    xylem_loop_run(_loop);

    xylem_dtls_ctx_destroy(ctx);
    xylem_loop_destroy(_loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
