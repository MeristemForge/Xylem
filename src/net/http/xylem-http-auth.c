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

#include "xylem/net/http/xylem-http-auth.h"

#include "xylem/net/http/xylem-http-router.h"

#include "xylem/encoding/xylem-base64.h"

#include "runtime/precond.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int xylem_http_basic_auth_size(int ulen, int plen) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_basic_auth_size");

    return 6 + xylem_base64_encode_size(ulen + 1 + plen) + 1;
}

int xylem_http_basic_auth(
    const char* user,
    const char* pass,
    char*       buf,
    int         buflen) {
    if (!user || !pass || !buf) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_basic_auth");

    int ulen     = (int)strlen(user);
    int plen     = (int)strlen(pass);
    int cred_len = ulen + 1 + plen;
    int b64_size = xylem_base64_encode_size(cred_len);
    int need     = 6 + b64_size + 1;
    if (buflen < need) {
        return -1;
    }
    memcpy(buf, "Basic ", 6);
    /* place credential at tail so forward encode won't overwrite unread input */
    char* cred = buf + 6 + b64_size - cred_len;
    memcpy(cred, user, (size_t)ulen);
    cred[ulen] = ':';
    memcpy(cred + ulen + 1, pass, (size_t)plen);
    int enc_len = xylem_base64_encode_std(
        (const uint8_t*)cred, cred_len,
        (uint8_t*)(buf + 6), b64_size + 1);
    if (enc_len < 0) {
        return -1;
    }
    buf[6 + enc_len] = '\0';
    return 6 + enc_len;
}

static bool _basic_auth_ok(const xylem_http_basic_auth_cfg_t* cfg,
                           const char* auth) {
    if (!auth || strncmp(auth, "Basic ", 6) != 0) {
        return false;
    }

    const char* b64 = auth + 6;
    int b64_len = (int)strlen(b64);
    int dec_cap = xylem_base64_decode_size(b64_len);
    if (dec_cap <= 0) {
        return false;
    }

    char decoded[256];
    if (dec_cap >= (int)sizeof(decoded)) {
        return false;
    }

    int dec_len = xylem_base64_decode_std(
        (const uint8_t*)b64, b64_len, (uint8_t*)decoded, (int)sizeof(decoded));
    if (dec_len <= 0) {
        return false;
    }
    decoded[dec_len] = '\0';

    char* colon = strchr(decoded, ':');
    if (!colon) {
        return false;
    }
    *colon = '\0';
    const char* user = decoded;
    const char* pass = colon + 1;

    return cfg->check(user, pass, cfg->userdata) != 0;
}

void xylem_http_basic_auth_middleware(
    xylem_http_writer_t* writer,
    xylem_http_req_t*    req,
    xylem_http_next_t*   next,
    void*                userdata) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_basic_auth_middleware");

    const xylem_http_basic_auth_cfg_t* cfg =
        (const xylem_http_basic_auth_cfg_t*)userdata;
    const char* realm = cfg->realm ? cfg->realm : "Restricted";

    const char* auth = xylem_http_req_header(req, "Authorization");
    if (_basic_auth_ok(cfg, auth)) {
        xylem_http_next_run(next);
        return;
    }

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "Basic realm=\"%s\"", realm);
    xylem_http_writer_set_status(writer, 401);
    xylem_http_writer_set_header(writer, "WWW-Authenticate", hdr);
    xylem_http_writer_write(writer, "Unauthorized", 12);
}
