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
 * UDP Echo Client
 *
 * Sends "hello" to 127.0.0.1:9001, waits for the echo, then exits.
 *
 * Usage: udp-echo-client
 * Pair:  udp-echo-server
 */

#include "xylem.h"
#include "xylem/net/xylem-udp.h"

#define SERVER_PORT 9001

static void _on_read(xylem_udp_t* udp, void* data, size_t len,
                     const char* host, uint16_t port) {
    (void)udp; (void)host; (void)port;
    xylem_logi("echo: %.*s", (int)len, (char*)data);
    xylem_shutdown();
}

static void _client_main(void* arg) {
    (void)arg;

    xylem_udp_handler_t handler = {
        .on_read = _on_read,
    };

    xylem_udp_t* udp = xylem_udp_dial("127.0.0.1", SERVER_PORT, &handler);
    if (!udp) {
        xylem_loge("failed to connect to 127.0.0.1:%d", SERVER_PORT);
        xylem_shutdown();
        return;
    }

    xylem_udp_send(udp, NULL, 0, "hello", 5);
    xylem_logi("sent: hello");
}

int main(void) {
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, 0);
    xylem_run(_client_main, NULL, NULL);
    xylem_logger_deinit();
    return 0;
}
