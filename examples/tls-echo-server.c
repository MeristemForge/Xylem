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
 * TLS Echo Server
 *
 * Listens on 127.0.0.1:9443 with TLS and echoes back every message.
 * If cert.pem / key.pem are missing, generates a self-signed certificate
 * automatically via the openssl command-line tool.
 *
 * Usage: tls-echo-server
 * Pair:  tls-echo-client
 */

#include "xylem.h"
#include "xylem/xylem-tls.h"
#include <stdio.h>
#include <stdlib.h>

#define LISTEN_PORT 9443
#define CERT_FILE   "cert.pem"
#define KEY_FILE    "key.pem"

static int _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return 0;
    }

    xylem_logi("generating self-signed certificate...");

    /**
     * Write a minimal openssl config to avoid dependency on the
     * compiled-in OPENSSLDIR path, which may not exist.
     */
    const char* cnf = "_xylem_tmp.cnf";
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
        " -config _xylem_tmp.cnf");
    remove(cnf);
    return (rc == 0) ? 0 : -1;
}

static void _on_accept(xylem_tls_t* tls) {
    (void)tls;
    xylem_logi("tls client connected");
}

static void _on_read(xylem_tls_t* tls, void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (char*)data);
    xylem_tls_send(tls, data, len);
}

static void _on_close(xylem_tls_t* tls, int err) {
    (void)tls;
    xylem_logi("tls client disconnected (err=%d)", err);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t* loop = xylem_loop_create();

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    if (!ctx) {
        xylem_loge("failed to create tls context");
        return 1;
    }

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate certificate (is openssl installed?)");
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    if (xylem_tls_ctx_load_cert(ctx, CERT_FILE, KEY_FILE) != 0) {
        xylem_loge("failed to load %s / %s", CERT_FILE, KEY_FILE);
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    xylem_tls_ctx_set_verify(ctx, false);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", LISTEN_PORT, &addr);

    xylem_tls_handler_t handler = {
        .on_accept = _on_accept,
        .on_read   = _on_read,
        .on_close  = _on_close,
    };

    xylem_tls_server_t* server = xylem_tls_listen(loop, &addr, ctx,
                                                  &handler, NULL);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    xylem_logi("tls echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_tls_ctx_destroy(ctx);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
