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
 * UDP Echo Server
 *
 * Binds to 127.0.0.1:9001 and echoes every datagram back to its sender.
 *
 * Usage: udp-echo-server
 * Pair:  udp-echo-client
 */

#include "xylem/xylem-udp.h"

#define LISTEN_PORT 9001

static void _on_read(xylem_udp_t* udp, void* data, size_t len,
                     xylem_addr_t* addr) {
    char host[64];
    uint16_t port;
    xylem_addr_ntop(addr, host, sizeof(host), &port);
    xylem_logi("recv %zu bytes from %s:%u", len, host, port);

    xylem_udp_send(udp, addr, data, len);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t loop;
    xylem_loop_init(&loop);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", LISTEN_PORT, &addr);

    xylem_udp_handler_t handler = {
        .on_read = _on_read,
    };

    xylem_udp_t* udp = xylem_udp_listen(&loop, &addr, &handler);
    if (!udp) {
        xylem_loge("failed to bind on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("udp echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(&loop);

    xylem_loop_deinit(&loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
