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

/* Internal URL and header helpers for the WebSocket opening handshake. */

_Pragma("once")

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ws_url_s {
    bool     secure;
    char     host[256];
    uint16_t port;
    char     path[2048];
} ws_url_t;

typedef struct ws_header_s {
    char* name;
    char* value;
} ws_header_t;

/* Accept ws/wss URLs; default ports are 80/443 and the default path is /. */
extern int ws_url_parse(const char* url, ws_url_t* out);

extern bool ws_header_eq(const char* a, const char* b);
extern const char*
ws_header_find(const ws_header_t* headers, size_t count, const char* name);
/* Copies name/value; release the array with ws_headers_free(). */
extern int ws_header_add(
    ws_header_t** headers,
    size_t*       count,
    size_t*       cap,
    const char*   name,
    size_t        name_len,
    const char*   value,
    size_t        value_len);
extern void ws_headers_free(ws_header_t* headers, size_t count);

extern const uint8_t ws_lower_table[256];
