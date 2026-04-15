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
 * DTLS Echo Server
 *
 * Listens on 127.0.0.1:9444 with DTLS and echoes back every datagram.
 * If cert.pem / key.pem are missing, generates a self-signed certificate
 * automatically via the openssl command-line tool.
 *
 * Usage: dtls-echo-server
 * Pair:  dtls-echo-client
 */

#include "xylem.h"
#include "xylem/xylem-dtls.h"

#include <stdio.h>
#include <stdlib.h>

#define LISTEN_PORT 9444
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

static void _on_accept(xylem_dtls_server_t* server, xylem_dtls_t* dtls) {
    (void)server;
    (void)dtls;
    xylem_logi("dtls client connected");
}

static void _on_read(xylem_dtls_t* dtls, void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (char*)data);
    xylem_dtls_send(dtls, data, len);
}

static void _on_close(xylem_dtls_t* dtls, int err, const char* errmsg) {
    (void)dtls;
    (void)err;
    (void)errmsg;
    xylem_logi("dtls client disconnected");
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t* loop = xylem_loop_create();

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    if (!ctx) {
        xylem_loge("failed to create dtls context");
        return 1;
    }

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate certificate (is openssl installed?)");
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    if (xylem_dtls_ctx_load_cert(ctx, CERT_FILE, KEY_FILE) != 0) {
        xylem_loge("failed to load %s / %s", CERT_FILE, KEY_FILE);
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    xylem_dtls_ctx_set_verify(ctx, false);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", LISTEN_PORT, &addr);

    xylem_dtls_handler_t handler = {
        .on_accept = _on_accept,
        .on_read   = _on_read,
        .on_close  = _on_close,
    };

    xylem_dtls_server_t* server = xylem_dtls_listen(loop, &addr, ctx,
                                                    &handler);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    xylem_logi("dtls echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_dtls_ctx_destroy(ctx);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
