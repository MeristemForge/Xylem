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

#include "ws-utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t ws_lower_table[256] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y',
    'z', 91,  92,  93,  94,  95,  96,  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
    'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
    'x', 'y', 'z', 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
    135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
    210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224,
    225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
    255,
};

int ws_url_parse(const char* url, ws_url_t* out) {
    if (!url || !out) {
        return -1;
    }
    for (const unsigned char* c = (const unsigned char*)url; *c; ++c) {
        if (*c <= 32 || *c == 127) {
            return -1;
        }
    }
    memset(out, 0, sizeof(*out));
    const char* p;
    if (!strncmp(url, "wss://", 6)) {
        out->secure = true;
        p           = url + 6;
    } else if (!strncmp(url, "ws://", 5)) {
        p = url + 5;
    } else {
        return -1;
    }
    const char* begin = p;
    const char* end;
    if (*p == '[') {
        begin = ++p;
        end   = strchr(p, ']');
        if (!end) {
            return -1;
        }
        p = end + 1;
    } else {
        end = p + strcspn(p, ":/?#");
        p   = end;
    }
    size_t n = (size_t)(end - begin);
    if (!n || n >= sizeof(out->host)) {
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)begin[i];
        if (c == '@' || c == '\\' || c == '[' || c == ']') {
            return -1;
        }
        out->host[i] = (char)ws_lower_table[c];
    }
    out->port = out->secure ? 443 : 80;
    if (*p == ':') {
        ++p;
        uint32_t port = 0;
        if (*p < '0' || *p > '9') {
            return -1;
        }
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned)(*p++ - '0');
            if (port > 65535) {
                return -1;
            }
        }
        if (!port) {
            return -1;
        }
        out->port = (uint16_t)port;
    }
    if (*p && *p != '/' && *p != '?' && *p != '#') {
        return -1;
    }
    size_t prefix = *p == '?' ? 1 : 0;
    size_t len    = strcspn(p, "#");
    if (!len) {
        snprintf(out->path, sizeof(out->path), "/");
        return 0;
    }
    if (prefix + len >= sizeof(out->path)) {
        return -1;
    }
    if (prefix) {
        out->path[0] = '/';
    }
    memcpy(out->path + prefix, p, len);
    out->path[prefix + len] = 0;
    return 0;
}

bool ws_header_eq(const char* a, const char* b) {
    /* Fast path: if lengths differ, strings can't be equal. */
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) {
        return false;
    }
    for (size_t i = 0; i < la; i++) {
        if (ws_lower_table[(uint8_t)a[i]] != ws_lower_table[(uint8_t)b[i]]) {
            return false;
        }
    }
    return true;
}

const char*
ws_header_find(const ws_header_t* headers, size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (ws_header_eq(headers[i].name, name)) {
            return headers[i].value;
        }
    }
    return NULL;
}

int ws_header_add(
    ws_header_t** headers,
    size_t*       count,
    size_t*       cap,
    const char*   name,
    size_t        name_len,
    const char*   value,
    size_t        value_len) {
    if (*count >= *cap) {
        size_t       new_cap = (*cap == 0) ? 16 : *cap * 2;
        ws_header_t* tmp =
            (ws_header_t*)realloc(*headers, new_cap * sizeof(ws_header_t));
        if (!tmp) {
            return -1;
        }
        *headers = tmp;
        *cap     = new_cap;
    }

    /* Single allocation for both name and value strings. */
    char* block = (char*)malloc(name_len + 1 + value_len + 1);
    if (!block) {
        return -1;
    }
    memcpy(block, name, name_len);
    block[name_len] = '\0';
    char* v         = block + name_len + 1;
    memcpy(v, value, value_len);
    v[value_len] = '\0';

    (*headers)[*count].name  = block;
    (*headers)[*count].value = v;
    (*count)++;
    return 0;
}

void ws_headers_free(ws_header_t* headers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        /**
         * name and value share a single allocation; value is inside the block.
         */
        free(headers[i].name);
    }
    free(headers);
}
