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

#include "xylem/net/xylem-ws.h"

#include "xylem/crypto/xylem-sha1.h"
#include "xylem/encoding/xylem-base64.h"
#include "xylem/xylem-utils.h"

#include "ws-deflate.h"
#include "ws-handshake.h"
#include "ws-utils.h"
#include "ws.h"

#include "llhttp/llhttp.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    ws_header_t*      headers;
    size_t            count, cap;
    char              name[256], value[4096];
    size_t            name_len, value_len;
} _ws_handshake_t;

static const char _ws_handshake_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static int
_ws_handshake_header_field_cb(llhttp_t* p, const char* at, size_t n) {
    _ws_handshake_t* h = p->data;
    if (n >= sizeof(h->name) - h->name_len) {
        return -1;
    }
    memcpy(h->name + h->name_len, at, n);
    h->name_len += n;
    return 0;
}

static int
_ws_handshake_header_value_cb(llhttp_t* p, const char* at, size_t n) {
    _ws_handshake_t* h = p->data;
    if (n >= sizeof(h->value) - h->value_len) {
        return -1;
    }
    memcpy(h->value + h->value_len, at, n);
    h->value_len += n;
    return 0;
}

static int _ws_handshake_header_complete_cb(llhttp_t* p) {
    _ws_handshake_t* h     = p->data;
    h->name[h->name_len]   = 0;
    h->value[h->value_len] = 0;
    if (h->count >= 64 || ws_header_find(h->headers, h->count, h->name)) {
        return -1;
    }
    int rc = ws_header_add(
        &h->headers,
        &h->count,
        &h->cap,
        h->name,
        h->name_len,
        h->value,
        h->value_len);
    h->name_len = h->value_len = 0;
    return rc;
}

static int _ws_handshake_headers_complete_cb(llhttp_t* p) {
    (void)p;
    return 2;
}

static bool _ws_handshake_has_token(const char* value, const char* token) {
    if (!value) {
        return false;
    }
    size_t wanted = strlen(token);
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') {
            ++value;
        }
        const char* end = strchr(value, ',');
        if (!end) {
            end = value + strlen(value);
        }
        const char* trim = end;
        while (trim > value && (trim[-1] == ' ' || trim[-1] == '\t')) {
            --trim;
        }
        if ((size_t)(trim - value) == wanted) {
            size_t i = 0;
            for (; i < wanted; ++i) {
                if (ws_lower_table[(uint8_t)value[i]] !=
                    ws_lower_table[(uint8_t)token[i]]) {
                    break;
                }
            }
            if (i == wanted) {
                return true;
            }
        }
        value = *end ? end + 1 : end;
    }
    return false;
}

static const char*
_ws_handshake_get_header(_ws_handshake_t* h, const char* name) {
    return ws_header_find(h->headers, h->count, name);
}

static int _ws_handshake_read(
    const ws_transport_t* t,
    bool                  server,
    _ws_handshake_t*      h,
    char                  buf[WS_HANDSHAKE_MAX_BYTES],
    size_t*               used,
    size_t*               total,
    uint64_t              deadline) {
    memset(h, 0, sizeof(*h));
    *total = 0;
    llhttp_settings_init(&h->settings);
    h->settings.on_header_field          = _ws_handshake_header_field_cb;
    h->settings.on_header_value          = _ws_handshake_header_value_cb;
    h->settings.on_header_value_complete = _ws_handshake_header_complete_cb;
    h->settings.on_headers_complete      = _ws_handshake_headers_complete_cb;
    llhttp_init(
        &h->parser,
        server ? HTTP_REQUEST : HTTP_RESPONSE,
        &h->settings);
    h->parser.data = h;
    t->set_rd_deadline(t->conn, deadline);
    t->set_wr_deadline(t->conn, deadline);
    while (*total < WS_HANDSHAKE_MAX_BYTES) {
        int n = t->read(
            t->conn,
            buf + *total,
            (int)(WS_HANDSHAKE_MAX_BYTES - *total));
        if (n <= 0) {
            return -1;
        }
        llhttp_errno_t err =
            llhttp_execute(&h->parser, buf + *total, (size_t)n);
        *total += (size_t)n;
        if (err == HPE_PAUSED_UPGRADE) {
            *used = (size_t)(llhttp_get_error_pos(&h->parser) - buf);
            return 0;
        }
        if (err != HPE_OK) {
            return -1;
        }
    }
    return -1;
}

static void _ws_handshake_deinit(_ws_handshake_t* h) {
    ws_headers_free(h->headers, h->count);
}

static int _ws_handshake_gen_key(char* out, size_t out_size) {
    /* Base64 of 16 bytes = 24 chars + null terminator = 25 */
    if (out == NULL || out_size < 25) {
        return -1;
    }

    uint8_t raw[16];
    for (int i = 0; i < 16; i++) {
        raw[i] = (uint8_t)xylem_utils_getprng(0, 255);
    }

    uint8_t enc_buf[32];
    int     written =
        xylem_base64_encode_std(raw, 16, enc_buf, (int)sizeof(enc_buf));
    if (written < 0) {
        return -1;
    }

    memcpy(out, enc_buf, (size_t)written);
    out[written] = '\0';
    return 0;
}

static char* _ws_handshake_build_request(
    const char* host,
    uint16_t    port,
    const char* path,
    const char* key,
    const char* extensions,
    size_t*     out_len) {
    if (!host || !path || !key) {
        return NULL;
    }
    char port_text[8] = "";
    if (port != 80 && port != 443) {
        snprintf(port_text, sizeof(port_text), ":%" PRIu16, port);
    }
    size_t capacity = strlen(path) + strlen(host) + strlen(key) +
                      (extensions ? strlen(extensions) : 0) + 320;
    char*  buf      = (char*)malloc(capacity);
    if (!buf) {
        return NULL;
    }
    int n = snprintf(
        buf,
        capacity,
        "GET %s HTTP/1.1\r\n"
        "Host: %s%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s%s%s\r\n",
        path,
        host,
        port_text,
        key,
        extensions ? "Sec-WebSocket-Extensions: " : "",
        extensions ? extensions : "",
        extensions ? "\r\n" : "");
    if (n < 0 || (size_t)n >= capacity) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)n;
    return buf;
}

static int _ws_handshake_prepare_server(
    _ws_handshake_t*       h,
    const xylem_ws_opts_t* opts,
    ws_handshake_result_t* result,
    ws_handshake_reply_t*  reply) {
    const char* key     = _ws_handshake_get_header(h, "Sec-WebSocket-Key");
    const char* version = _ws_handshake_get_header(h, "Sec-WebSocket-Version");
    const char* length  = _ws_handshake_get_header(h, "Content-Length");
    uint8_t     decoded[32];
    if (h->parser.method != HTTP_GET || h->parser.http_major != 1 ||
        h->parser.http_minor != 1 || !_ws_handshake_get_header(h, "Host") ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(h, "Upgrade"),
            "websocket") ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(h, "Connection"),
            "upgrade") ||
        !version || strcmp(version, "13") || !key || strlen(key) != 24 ||
        xylem_base64_decode_std(
            (const uint8_t*)key,
            24,
            decoded,
            sizeof(decoded)) != 16 ||
        _ws_handshake_get_header(h, "Transfer-Encoding") ||
        (length && strcmp(length, "0"))) {
        return -1;
    }
    char accept[29], extensions[160] = "", ext[128];
    if (ws_handshake_compute_accept(key, accept, sizeof(accept))) {
        return -1;
    }
    ws_deflate_offer_t offer = {0};

    if (opts && opts->permessage_deflate) {
        const char* value =
            _ws_handshake_get_header(h, "Sec-WebSocket-Extensions");
        if (value && !ws_deflate_parse_offer(value, &offer)) {
            if (!opts->deflate_context_takeover) {
                offer.server_no_context_takeover = true;
                offer.client_no_context_takeover = true;
            }
            if (!ws_deflate_build_server_accept(&offer, ext, sizeof(ext))) {
                snprintf(
                    extensions,
                    sizeof(extensions),
                    "Sec-WebSocket-Extensions: %s\r\n",
                    ext);
                result->deflate             = true;
                result->no_context_takeover = offer.server_no_context_takeover;
            }
        }
    }
    int n = snprintf(
        reply->data,
        sizeof(reply->data),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
        "%s\r\n%s\r\n",
        accept,
        extensions);
    if (n < 0 || (size_t)n >= sizeof(reply->data)) {
        return -1;
    }
    reply->len = (size_t)n;
    return 0;
}

static int _ws_handshake_validate_server(
    _ws_handshake_t*       h,
    const char*            key,
    bool                   offered,
    bool                   context_takeover,
    ws_handshake_result_t* result) {
    const char* accept = _ws_handshake_get_header(h, "Sec-WebSocket-Accept");
    if (h->parser.status_code != 101 || h->parser.http_major != 1 ||
        h->parser.http_minor != 1 ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(h, "Upgrade"),
            "websocket") ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(h, "Connection"),
            "upgrade") ||
        !accept || ws_handshake_validate_accept(key, accept)) {
        return -1;
    }
    const char* value = _ws_handshake_get_header(h, "Sec-WebSocket-Extensions");
    ws_deflate_offer_t server = {0};
    if (value && (!offered || ws_deflate_parse_offer(value, &server))) {
        return -1;
    }
    result->deflate = value != NULL;
    result->no_context_takeover =
        value && (server.server_no_context_takeover || !context_takeover);
    return 0;
}

int ws_handshake_compute_accept(const char* key, char* out, size_t out_size) {
    /* Accept = Base64(SHA-1(key + GUID)), output is 28 chars + null = 29 */
    if (key == NULL || out == NULL || out_size < 29) {
        return -1;
    }

    size_t key_len  = strlen(key);
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
    int     written =
        xylem_base64_encode_std(digest, 20, enc_buf, (int)sizeof(enc_buf));
    if (written < 0) {
        return -1;
    }

    memcpy(out, enc_buf, (size_t)written);
    out[written] = '\0';
    return 0;
}

int ws_handshake_validate_accept(
    const char* expected_key,
    const char* accept_value) {
    if (expected_key == NULL || accept_value == NULL) {
        return -1;
    }

    char computed[29];
    if (ws_handshake_compute_accept(expected_key, computed, sizeof(computed)) !=
        0) {
        return -1;
    }

    if (strcmp(computed, accept_value) != 0) {
        return -1;
    }
    return 0;
}

int ws_handshake_server_prepare(
    const ws_transport_t*  t,
    const xylem_ws_opts_t* opts,
    ws_handshake_result_t* result,
    ws_handshake_reply_t*  reply) {
    result->pending_len         = 0;
    result->deflate             = false;
    result->no_context_takeover = false;
    reply->len                  = 0;
    uint64_t timeout            = opts && opts->handshake_timeout_ms
                                      ? opts->handshake_timeout_ms
                                      : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    reply->deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;
    _ws_handshake_t h;
    size_t          used = 0, total = 0;
    int             rc = _ws_handshake_read(
        t,
        true,
        &h,
        result->pending,
        &used,
        &total,
        reply->deadline);
    if (!rc) {
        rc = _ws_handshake_prepare_server(&h, opts, result, reply);
    }
    if (!rc) {
        result->pending_len = total - used;
        memmove(result->pending, result->pending + used, result->pending_len);
    }
    _ws_handshake_deinit(&h);
    return rc;
}

int ws_handshake_server_finish(
    const ws_transport_t*       t,
    const ws_handshake_reply_t* reply) {
    if (xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= reply->deadline ||
        t->write(t->conn, reply->data, (int)reply->len)) {
        return -1;
    }
    t->set_rd_deadline(t->conn, 0);
    t->set_wr_deadline(t->conn, 0);
    return 0;
}

int ws_handshake_client(
    const ws_transport_t*  t,
    const char*            host,
    uint16_t               port,
    const char*            path,
    const xylem_ws_opts_t* opts,
    ws_handshake_result_t* result) {
    result->pending_len         = 0;
    result->deflate             = false;
    result->no_context_takeover = false;
    char        key[25], offer[128];
    const char* extension = NULL;
    if (_ws_handshake_gen_key(key, sizeof(key))) {
        return -1;
    }
    if (opts && opts->permessage_deflate &&
        !ws_deflate_build_client_offer(
            opts->deflate_context_takeover,
            offer,
            sizeof(offer))) {
        extension = offer;
    }
    size_t len;
    char*  request =
        _ws_handshake_build_request(host, port, path, key, extension, &len);
    if (!request) {
        return -1;
    }
    uint64_t timeout  = opts && opts->handshake_timeout_ms
                            ? opts->handshake_timeout_ms
                            : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;
    t->set_wr_deadline(t->conn, deadline);
    int rc = t->write(t->conn, request, (int)len);
    free(request);
    if (rc) {
        return -1;
    }
    if (xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
        return -1;
    }
    _ws_handshake_t h;
    size_t          used = 0, total = 0;
    rc = _ws_handshake_read(
        t,
        false,
        &h,
        result->pending,
        &used,
        &total,
        deadline);
    if (!rc) {
        rc = _ws_handshake_validate_server(
            &h,
            key,
            extension != NULL,
            opts && opts->deflate_context_takeover,
            result);
    }
    if (!rc) {
        result->pending_len = total - used;
        memmove(result->pending, result->pending + used, result->pending_len);
        t->set_rd_deadline(t->conn, 0);
        t->set_wr_deadline(t->conn, 0);
    }
    _ws_handshake_deinit(&h);
    return rc;
}
