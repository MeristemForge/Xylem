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

#include "xylem/net/http/xylem-http-form.h"

#include "xylem/encoding/xylem-url.h"

#include "runtime/precond.h"

#include <string.h>

static void _form_decode_inplace(char* s, size_t len, size_t* out_len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '+') {
            s[i] = ' ';
        }
    }
    int n = xylem_url_decode(
        (const uint8_t*)s, (int)len, (uint8_t*)s, (int)len);
    *out_len = n > 0 ? (size_t)n : len;
}

int xylem_http_form_count(const char* body, size_t body_len) {
    if (!body || body_len == 0) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_form_count");

    int count = 1;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '&') {
            count++;
        }
    }
    return count;
}

int xylem_http_form_parse(
    char*             buf,
    size_t            buf_len,
    xylem_http_hdr_t* pairs,
    int               max_pairs) {
    if (!buf || !pairs || max_pairs <= 0) {
        return -1;
    }
    if (buf_len == 0) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_form_parse");

    int    count = 0;
    char*  pos   = buf;
    char*  end   = buf + buf_len;

    while (pos < end && count < max_pairs) {
        char* amp = (char*)memchr(pos, '&', (size_t)(end - pos));
        size_t seg_len = amp ? (size_t)(amp - pos) : (size_t)(end - pos);

        char* eq = (char*)memchr(pos, '=', seg_len);

        char*  key     = pos;
        size_t key_len = eq ? (size_t)(eq - pos) : seg_len;
        char*  val     = eq ? eq + 1 : pos + seg_len;
        size_t val_len = eq ? seg_len - key_len - 1 : 0;

        size_t dk, dv;
        _form_decode_inplace(key, key_len, &dk);
        key[dk] = '\0';
        _form_decode_inplace(val, val_len, &dv);
        val[dv] = '\0';

        pairs[count].name  = key;
        pairs[count].value = val;
        count++;

        pos = amp ? amp + 1 : end;
    }

    return count;
}

const char* xylem_http_form_get(
    const xylem_http_hdr_t* pairs,
    int                     count,
    const char*             key) {
    if (!pairs || !key) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_form_get");

    for (int i = 0; i < count; i++) {
        if (strcmp(pairs[i].name, key) == 0) {
            return pairs[i].value;
        }
    }
    return NULL;
}
