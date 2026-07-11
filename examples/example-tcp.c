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
 * TCP Echo Example (coroutine)
 *
 * Demonstrates coroutine-based TCP networking in server or client mode.
 *
 * Usage: tcp-echo <ip> <port> <server|client>
 */

#include "xylem.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 64

typedef struct {
    const char* ip;
    uint16_t    port;
    bool        server;
} _config_t;

static void _usage(const char* program) {
    xylem_loge("usage: %s <ip> <port> <server|client>", program);
}

static int _parse_port(const char* text, uint16_t* port) {
    char* end   = NULL;
    long  value = strtol(text, &end, 10);
    if (!text[0] || *end || value < 1 || value > UINT16_MAX) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static void _handle_client(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char              buf[BUFFER_SIZE];
    int               n;
    while ((n = xylem_tcp_read(conn, buf, sizeof(buf))) > 0) {
        xylem_tcp_write(conn, buf, n);
    }
    xylem_tcp_close(conn);
}

static void _server(void* arg) {
    _config_t* config = (_config_t*)arg;

    xylem_tcp_listener_t* ln
        = xylem_tcp_listen(config->ip, config->port, NULL);
    xylem_logi("[server] listening on %s:%u", config->ip, config->port);

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
        xylem_spawn(_handle_client, conn);
    }
}

static void _client(void* arg) {
    _config_t* config = (_config_t*)arg;

    xylem_tcp_conn_t* conn
        = xylem_tcp_dial(config->ip, config->port, NULL);
    xylem_logi("[client] connected");

    xylem_tcp_write(conn, "hello", 5);

    char buf[BUFFER_SIZE] = {0};
    xylem_tcp_read(conn, buf, sizeof(buf) - 1);
    xylem_logi("[client] echo: %s", buf);

    xylem_tcp_close(conn);
}

static void _main(void* arg) {
    _config_t* config = (_config_t*)arg;
    if (config->server) {
        _server(config);
    } else {
        _client(config);
    }
}

int main(int argc, char** argv) {
    xylem_logger_init(NULL, NULL);

    if (argc != 4) {
        _usage(argv[0]);
        xylem_logger_deinit();
        return -1;
    }

    _config_t config = {.ip = argv[1]};
    if (_parse_port(argv[2], &config.port) != 0) {
        _usage(argv[0]);
        xylem_logger_deinit();
        return -1;
    }
    if (strcmp(argv[3], "server") == 0) {
        config.server = true;
    } else if (strcmp(argv[3], "client") == 0) {
        config.server = false;
    } else {
        _usage(argv[0]);
        xylem_logger_deinit();
        return -1;
    }

    xylem_run(_main, &config, NULL);
    xylem_logger_deinit();
    return 0;
}
