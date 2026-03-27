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
 * WebSocket Secure Echo Server
 *
 * Listens on 127.0.0.1:9003 with TLS and echoes back every WebSocket
 * message. Automatically generates a self-signed certificate if none
 * exists in the current directory.
 *
 * Usage: wss-echo-server
 * Test:  wss-echo-client
 */

#include "xylem.h"
#include "xylem/ws/xylem-ws-server.h"
#include <stdio.h>
#include <stdlib.h>

#define LISTEN_PORT 9003
#define CERT_FILE   "wss-cert.pem"
#define KEY_FILE    "wss-key.pem"

static int _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return 0;
    }

    xylem_logi("generating self-signed certificate...");

    const char* cnf = "_xylem_wss_tmp.cnf";
    f = fopen(cnf, "w");
    if (!f) {
        return -1;
    }
    fprintf(f,
            "[req]\n"
            "distinguished_name=dn\n"
            "prompt=no\n"
            "[dn]\n"
            "CN=localhost\n");
    fclose(f);

    int rc = system(
        "openssl req -x509 -newkey rsa:2048"
        " -keyout " KEY_FILE " -out " CERT_FILE
        " -days 365 -nodes"
        " -config _xylem_wss_tmp.cnf");
    remove(cnf);
    return (rc == 0) ? 0 : -1;
}

static void _on_accept(xylem_ws_conn_t* conn) {
    (void)conn;
    xylem_logi("tls client connected");
}

static void _on_message(xylem_ws_conn_t* conn,
                        xylem_ws_opcode_t opcode,
                        const void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (const char*)data);
    xylem_ws_send(conn, opcode, data, len);
}

static void _on_close(xylem_ws_conn_t* conn,
                      uint16_t code, const char* reason, size_t reason_len) {
    (void)conn;
    (void)reason;
    (void)reason_len;
    xylem_logi("tls client disconnected (code=%u)", code);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate certificate (is openssl in PATH?)");
        return 1;
    }

    xylem_loop_t* loop = xylem_loop_create();

    xylem_ws_handler_t handler = {
        .on_accept  = _on_accept,
        .on_message = _on_message,
        .on_close   = _on_close,
    };

    xylem_ws_srv_cfg_t cfg = {
        .host     = "127.0.0.1",
        .port     = LISTEN_PORT,
        .handler  = &handler,
        .tls_cert = CERT_FILE,
        .tls_key  = KEY_FILE,
    };

    xylem_ws_server_t* server = xylem_ws_listen(loop, &cfg);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("wss echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
