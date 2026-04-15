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

#include "xylem/xylem-base64.h"
#include "xylem/xylem-sha1.h"
#include "xylem/xylem-utils.h"

#include "ws-handshake.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char _ws_handshake_guid[] = "258EAFA5-E914-47DA-95CA-5AB9DC63B5E0";

int ws_handshake_gen_key(char* out, size_t out_size) {
    /* Base64 of 16 bytes = 24 chars + null terminator = 25 */
    if (out == NULL || out_size < 25) {
        return -1;
    }

    uint8_t raw[16];
    for (int i = 0; i < 16; i++) {
        raw[i] = (uint8_t)xylem_utils_getprng(0, 255);
    }

    uint8_t enc_buf[32];
    int written = xylem_base64_encode_std(raw, 16, enc_buf, (int)sizeof(enc_buf));
    if (written < 0) {
        return -1;
    }

    memcpy(out, enc_buf, (size_t)written);
    out[written] = '\0';
    return 0;
}

int ws_handshake_compute_accept(const char* key,
                                char* out, size_t out_size) {
    /* Accept = Base64(SHA-1(key + GUID)), output is 28 chars + null = 29 */
    if (key == NULL || out == NULL || out_size < 29) {
        return -1;
    }

    size_t key_len = strlen(key);
    size_t guid_len = sizeof(_ws_handshake_guid) - 1;

    xylem_sha1_t* ctx = xylem_sha1_create();
    if (ctx == NULL) {
        return -1;
    }

    xylem_sha1_update(ctx, (const uint8_t*)key, key_len);
    xylem_sha1_update(ctx, (const uint8_t*)_ws_handshake_guid, guid_len);

    uint8_t digest[20];
    xylem_sha1_final(ctx, digest);
    xylem_sha1_destroy(ctx);

    /* Base64 of 20 bytes = 28 chars */
    uint8_t enc_buf[32];
    int written = xylem_base64_encode_std(digest, 20, enc_buf, (int)sizeof(enc_buf));
    if (written < 0) {
        return -1;
    }

    memcpy(out, enc_buf, (size_t)written);
    out[written] = '\0';
    return 0;
}

char* ws_handshake_build_request(const char* host, uint16_t port,
                                 const char* path, const char* key,
                                 size_t* out_len) {
    if (host == NULL || path == NULL || key == NULL) {
        return NULL;
    }

    /**
     * Build: GET <path> HTTP/1.1\r\n
     *        Host: <host>:<port>\r\n
     *        Upgrade: websocket\r\n
     *        Connection: Upgrade\r\n
     *        Sec-WebSocket-Key: <key>\r\n
     *        Sec-WebSocket-Version: 13\r\n
     *        \r\n
     */
    size_t host_len = strlen(host);
    size_t path_len = strlen(path);
    size_t key_len = strlen(key);

    /* Conservative upper bound for the request size */
    size_t buf_size = path_len + host_len + key_len + 256;
    char* buf = (char*)malloc(buf_size);
    if (buf == NULL) {
        return NULL;
    }

    /* Omit port from Host header when it matches the scheme default */
    int len;
    if ((port == 80) || (port == 443)) {
        len = snprintf(buf, buf_size,
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "\r\n",
                       path, host, key);
    } else {
        len = snprintf(buf, buf_size,
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s:%" PRIu16 "\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "\r\n",
                       path, host, port, key);
    }

    if (len < 0 || (size_t)len >= buf_size) {
        free(buf);
        return NULL;
    }

    if (out_len != NULL) {
        *out_len = (size_t)len;
    }
    return buf;
}

int ws_handshake_validate_accept(const char* expected_key,
                                 const char* accept_value) {
    if (expected_key == NULL || accept_value == NULL) {
        return -1;
    }

    char computed[29];
    if (ws_handshake_compute_accept(expected_key, computed, sizeof(computed)) != 0) {
        return -1;
    }

    if (strcmp(computed, accept_value) != 0) {
        return -1;
    }
    return 0;
}

char* ws_handshake_build_response(const char* accept_value,
                                  size_t* out_len) {
    if (accept_value == NULL) {
        return NULL;
    }

    /**
     * Build: HTTP/1.1 101 Switching Protocols\r\n
     *        Upgrade: websocket\r\n
     *        Connection: Upgrade\r\n
     *        Sec-WebSocket-Accept: <accept_value>\r\n
     *        \r\n
     */
    size_t accept_len = strlen(accept_value);
    size_t buf_size = accept_len + 128;
    char* buf = (char*)malloc(buf_size);
    if (buf == NULL) {
        return NULL;
    }

    int len = snprintf(buf, buf_size,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n"
                       "\r\n",
                       accept_value);

    if (len < 0 || (size_t)len >= buf_size) {
        free(buf);
        return NULL;
    }

    if (out_len != NULL) {
        *out_len = (size_t)len;
    }
    return buf;
}
