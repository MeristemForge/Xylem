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

#include "xylem/net/http/xylem-http-multipart.h"

#include "runtime/precond.h"

#include <string.h>

static const char* _memmem(const char* hay, size_t hay_len,
                           const char* needle, size_t needle_len) {
    if (needle_len == 0) {
        return hay;
    }
    if (needle_len > hay_len) {
        return NULL;
    }
    size_t limit = hay_len - needle_len;
    for (size_t i = 0; i <= limit; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/* Find quoted value for a param in Content-Disposition, e.g. name="foo" */
static const char* _find_param(const char* hdr, size_t hdr_len,
                               const char* param, size_t param_len,
                               size_t* out_len) {
    const char* end = hdr + hdr_len;
    const char* p = _memmem(hdr, hdr_len, param, param_len);
    if (!p) {
        *out_len = 0;
        return NULL;
    }
    p += param_len;
    if (p >= end) {
        *out_len = 0;
        return NULL;
    }
    /* skip optional whitespace */
    while (p < end && *p == ' ') {
        p++;
    }
    if (p < end && *p == '"') {
        p++;
        const char* q = (const char*)memchr(p, '"', (size_t)(end - p));
        if (!q) {
            *out_len = 0;
            return NULL;
        }
        *out_len = (size_t)(q - p);
        return p;
    }
    /* unquoted: read until ; or end */
    const char* start = p;
    while (p < end && *p != ';' && *p != '\r' && *p != ' ') {
        p++;
    }
    *out_len = (size_t)(p - start);
    return start;
}

static void _parse_part_headers(const char* hdr_start, size_t hdr_len,
                                xylem_http_multipart_part_t* part) {
    part->name         = _find_param(hdr_start, hdr_len, "name=", 5,
                                     &part->name_len);
    part->filename     = _find_param(hdr_start, hdr_len, "filename=", 9,
                                     &part->filename_len);

    /* find Content-Type in part headers */
    const char* ct_key = "Content-Type:";
    size_t ct_key_len  = 13;
    const char* ct = _memmem(hdr_start, hdr_len, ct_key, ct_key_len);
    if (ct) {
        ct += ct_key_len;
        while (ct < hdr_start + hdr_len && *ct == ' ') {
            ct++;
        }
        const char* ct_end = (const char*)memchr(
            ct, '\r', (size_t)(hdr_start + hdr_len - ct));
        if (ct_end) {
            part->content_type     = ct;
            part->content_type_len = (size_t)(ct_end - ct);
        } else {
            part->content_type     = ct;
            part->content_type_len = (size_t)(hdr_start + hdr_len - ct);
        }
    } else {
        part->content_type     = NULL;
        part->content_type_len = 0;
    }
}

const char* xylem_http_multipart_boundary(
    const char* content_type,
    size_t*     len) {
    if (!content_type || !len) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_multipart_boundary");

    const char* key = "boundary=";
    const char* p = strstr(content_type, key);
    if (!p) {
        *len = 0;
        return NULL;
    }
    p += 9;
    if (*p == '"') {
        p++;
        const char* q = strchr(p, '"');
        if (!q) {
            *len = 0;
            return NULL;
        }
        *len = (size_t)(q - p);
        return p;
    }
    const char* start = p;
    while (*p && *p != ';' && *p != ' ' && *p != '\r' && *p != '\n') {
        p++;
    }
    *len = (size_t)(p - start);
    return start;
}

int xylem_http_multipart_parse(
    const void*                  data,
    size_t                       data_len,
    const char*                  boundary,
    xylem_http_multipart_part_t* parts,
    int                          max_parts) {
    if (!data || !boundary || !parts || max_parts <= 0) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_multipart_parse");

    size_t blen = strlen(boundary);
    if (blen == 0 || blen + 4 > data_len) {
        return -1;
    }

    /* Build delimiter: "\r\n--" + boundary */
    char delim[256];
    if (blen + 4 > sizeof(delim)) {
        return -1;
    }
    delim[0] = '\r';
    delim[1] = '\n';
    delim[2] = '-';
    delim[3] = '-';
    memcpy(delim + 4, boundary, blen);
    size_t dlen = blen + 4;

    const char* buf = (const char*)data;
    const char* end = buf + data_len;

    /* Find first boundary: "--" + boundary */
    const char* first_delim = _memmem(buf, data_len, delim + 2, dlen - 2);
    if (!first_delim) {
        return 0;
    }
    const char* pos = first_delim + dlen - 2;

    int count = 0;

    while (pos < end && count < max_parts) {
        /* After boundary: expect \r\n (more parts) or -- (end) */
        if (pos + 2 > end) {
            break;
        }
        if (pos[0] == '-' && pos[1] == '-') {
            break;
        }
        if (pos[0] == '\r' && pos[1] == '\n') {
            pos += 2;
        } else {
            break;
        }

        /* Find end of part headers (blank line: \r\n\r\n) */
        const char* hdr_end = _memmem(pos, (size_t)(end - pos), "\r\n\r\n", 4);
        if (!hdr_end) {
            break;
        }

        size_t hdr_len = (size_t)(hdr_end - pos);
        const char* body_start = hdr_end + 4;

        /* Find next boundary */
        const char* next = _memmem(body_start, (size_t)(end - body_start),
                                   delim, dlen);
        if (!next) {
            break;
        }

        memset(&parts[count], 0, sizeof(parts[count]));
        _parse_part_headers(pos, hdr_len, &parts[count]);
        parts[count].body     = body_start;
        parts[count].body_len = (size_t)(next - body_start);
        count++;

        pos = next + dlen;
    }

    return count;
}

int xylem_http_multipart_parse_request(
    const xylem_http_req_t*      req,
    xylem_http_multipart_part_t* parts,
    int                          max_parts) {
    if (!req || !parts || max_parts <= 0) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_multipart_parse_request");

    const char* ct = xylem_http_req_header(req, "Content-Type");
    if (!ct) {
        return -1;
    }
    size_t blen;
    const char* boundary = xylem_http_multipart_boundary(ct, &blen);
    if (!boundary || blen == 0) {
        return -1;
    }

    /* Temporary null-terminated boundary for parse */
    char bnd_buf[256];
    if (blen >= sizeof(bnd_buf)) {
        return -1;
    }
    memcpy(bnd_buf, boundary, blen);
    bnd_buf[blen] = '\0';

    return xylem_http_multipart_parse(
        xylem_http_req_body(req), xylem_http_req_body_len(req),
        bnd_buf, parts, max_parts);
}
