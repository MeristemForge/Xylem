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

#include "http-common.h"

#include "xylem/http/xylem-http-common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* xylem_http_url_encode(const char* src, size_t src_len,
                            size_t* out_len) {
    if (!src && src_len > 0) {
        return NULL;
    }

    size_t max_len = src_len * 3 + 1;
    char* out = malloc(max_len);
    if (!out) {
        return NULL;
    }

    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < src_len; i++) {
        uint8_t c = (uint8_t)src[i];
        if (http_is_unreserved(c)) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 0x0F];
        }
    }
    out[j] = '\0';

    if (out_len) {
        *out_len = j;
    }
    return out;
}

char* xylem_http_url_decode(const char* src, size_t src_len,
                            size_t* out_len) {
    if (!src && src_len > 0) {
        return NULL;
    }

    char* out = malloc(src_len + 1);
    if (!out) {
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            int hi = http_hex_digit(src[i + 1]);
            int lo = http_hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[j++] = src[i];
    }
    out[j] = '\0';

    if (out_len) {
        *out_len = j;
    }
    return out;
}


/* Check if origin matches the allowed_origins list. */
static bool _http_cors_origin_match(const char* allowed, const char* origin) {
    if (!allowed || !origin) {
        return false;
    }
    if (allowed[0] == '*' && allowed[1] == '\0') {
        return true;
    }

    size_t origin_len = strlen(origin);
    const char* p = allowed;

    while (*p) {
        /* Skip leading whitespace. */
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char* start = p;
        while (*p && *p != ',') {
            p++;
        }
        /* Trim trailing whitespace. */
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }

        size_t len = (size_t)(end - start);
        if (len == origin_len && memcmp(start, origin, len) == 0) {
            return true;
        }

        if (*p == ',') {
            p++;
        }
    }
    return false;
}

size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                               const char* origin,
                               bool is_preflight,
                               xylem_http_hdr_t* out,
                               size_t out_cap) {
    if (!cors || !origin || !out || out_cap == 0) {
        return 0;
    }

    if (!_http_cors_origin_match(cors->allowed_origins, origin)) {
        return 0;
    }

    size_t n = 0;

    /* Access-Control-Allow-Origin */
    if (n < out_cap) {
        out[n].name = "Access-Control-Allow-Origin";
        if (cors->allow_credentials) {
            /* Must echo actual origin, not "*". */
            out[n].value = origin;
        } else {
            out[n].value = cors->allowed_origins;
        }
        n++;
    }

    /* Access-Control-Allow-Credentials */
    if (cors->allow_credentials && n < out_cap) {
        out[n].name  = "Access-Control-Allow-Credentials";
        out[n].value = "true";
        n++;
    }

    /* Access-Control-Expose-Headers */
    if (cors->expose_headers && !is_preflight && n < out_cap) {
        out[n].name  = "Access-Control-Expose-Headers";
        out[n].value = cors->expose_headers;
        n++;
    }

    /* Preflight-only headers. */
    if (is_preflight) {
        if (cors->allowed_methods && n < out_cap) {
            out[n].name  = "Access-Control-Allow-Methods";
            out[n].value = cors->allowed_methods;
            n++;
        }
        if (cors->allowed_headers && n < out_cap) {
            out[n].name  = "Access-Control-Allow-Headers";
            out[n].value = cors->allowed_headers;
            n++;
        }
        if (cors->max_age > 0 && n < out_cap) {
            /* max_age is stored as int; the caller's cors struct
               must remain valid, so we use a static buffer approach.
               Since this is single-threaded per request, a small
               static buffer per call is fine -- but we need the value
               to persist. We use the fact that max_age fits in a
               fixed-size string. Caller must consume headers before
               the next call. */
            static char _cors_max_age_buf[16];
            snprintf(_cors_max_age_buf, sizeof(_cors_max_age_buf),
                     "%d", cors->max_age);
            out[n].name  = "Access-Control-Max-Age";
            out[n].value = _cors_max_age_buf;
            n++;
        }
    }

    /* When allowed_origins is not "*", the response varies by Origin.
       Caches must key on the Origin header to avoid serving a response
       allowed for one origin to a different origin. */
    if (!(cors->allowed_origins[0] == '*' &&
          cors->allowed_origins[1] == '\0') && n < out_cap) {
        out[n].name  = "Vary";
        out[n].value = "Origin";
        n++;
    }

    return n;
}


typedef struct {
    char*    name;
    char*    filename;
    char*    content_type;
    uint8_t* data;
    size_t   data_len;
} _http_multipart_part_t;

struct xylem_http_multipart_s {
    _http_multipart_part_t* parts;
    size_t                  count;
    size_t                  cap;
};

/* Extract boundary from Content-Type: multipart/form-data; boundary=XXX */
static const char* _http_multipart_boundary(const char* ct, size_t* out_len) {
    const char* p = strstr(ct, "boundary=");
    if (!p) {
        return NULL;
    }
    p += 9; /* strlen("boundary=") */
    const char* start = p;
    while (*p && *p != ';' && *p != ' ' && *p != '\t' && *p != '\r') {
        p++;
    }
    *out_len = (size_t)(p - start);
    return (*out_len > 0) ? start : NULL;
}

/* Extract a quoted attribute value from Content-Disposition. */
static char* _http_multipart_attr(const char* hdr, const char* attr) {
    size_t attr_len = strlen(attr);
    const char* p = hdr;

    while ((p = strstr(p, attr)) != NULL) {
        /* Ensure this is not a substring of another attribute.
           Check that the character before attr is a space, semicolon,
           or start of string. */
        if (p != hdr) {
            char prev = *(p - 1);
            if (prev != ' ' && prev != ';' && prev != '\t') {
                p += attr_len;
                continue;
            }
        }
        p += attr_len;
        if (*p != '=' || *(p + 1) != '"') {
            continue;
        }
        p += 2; /* skip =" */
        const char* start = p;
        while (*p && *p != '"') {
            p++;
        }
        size_t len = (size_t)(p - start);
        char* val = malloc(len + 1);
        if (!val) {
            return NULL;
        }
        memcpy(val, start, len);
        val[len] = '\0';
        return val;
    }
    return NULL;
}

/* Find Content-Type in part headers. */
static char* _http_multipart_ct(const char* headers, size_t hdr_len) {
    /* Search line by line for "Content-Type:" (case-insensitive). */
    const char* p = headers;
    const char* end = headers + hdr_len;

    while (p < end) {
        /* Skip to start of line. */
        const char* line = p;
        const char* line_end = line;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        size_t line_len = (size_t)(line_end - line);

        if (line_len > 13) {
            /* Check "Content-Type:" case-insensitively. */
            const char ct_lower[] = "content-type:";
            bool match = true;
            for (size_t i = 0; i < 13; i++) {
                char c = line[i];
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c + 32);
                }
                if (c != ct_lower[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                const char* v = line + 13;
                size_t remain = line_len - 13;
                while (remain > 0 && (*v == ' ' || *v == '\t')) {
                    v++;
                    remain--;
                }
                char* ct = malloc(remain + 1);
                if (!ct) {
                    return NULL;
                }
                memcpy(ct, v, remain);
                ct[remain] = '\0';
                return ct;
            }
        }

        /* Advance past line ending. */
        p = line_end;
        if (p < end && *p == '\r') {
            p++;
        }
        if (p < end && *p == '\n') {
            p++;
        }
    }
    return NULL;
}

xylem_http_multipart_t* xylem_http_multipart_parse(
    const char* content_type, const void* body, size_t body_len) {
    if (!content_type || !body || body_len == 0) {
        return NULL;
    }

    size_t bnd_len;
    const char* bnd = _http_multipart_boundary(content_type, &bnd_len);
    if (!bnd) {
        return NULL;
    }

    /* Build delimiter: "\r\n--" + boundary */
    size_t delim_len = 4 + bnd_len;
    char* delim = malloc(delim_len + 1);
    if (!delim) {
        return NULL;
    }
    memcpy(delim, "\r\n--", 4);
    memcpy(delim + 4, bnd, bnd_len);
    delim[delim_len] = '\0';

    xylem_http_multipart_t* mp = calloc(1, sizeof(*mp));
    if (!mp) {
        free(delim);
        return NULL;
    }

    const char* data = (const char*)body;
    const char* end  = data + body_len;

    /* Skip preamble: find first "--" + boundary. */
    const char* p = data;
    /* First boundary starts with "--" + boundary (no leading \r\n). */
    if (body_len < 2 + bnd_len || memcmp(p, "--", 2) != 0 ||
        memcmp(p + 2, bnd, bnd_len) != 0) {
        free(delim);
        free(mp);
        return NULL;
    }
    p += 2 + bnd_len;
    /* Skip \r\n after first boundary. */
    if (p + 2 <= end && p[0] == '\r' && p[1] == '\n') {
        p += 2;
    }

    while (p < end) {
        /* Find next delimiter. */
        const char* next = NULL;
        for (const char* s = p; s + delim_len <= end; s++) {
            if (memcmp(s, delim, delim_len) == 0) {
                next = s;
                break;
            }
        }
        if (!next) {
            break;
        }

        /* Part data is from p to next. Parse headers + body. */
        size_t part_len = (size_t)(next - p);
        const char* hdr_end = NULL;
        for (const char* s = p; s + 4 <= p + part_len; s++) {
            if (s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
                hdr_end = s;
                break;
            }
        }
        if (!hdr_end) {
            break;
        }

        size_t hdr_len = (size_t)(hdr_end - p);
        const char* part_body = hdr_end + 4;
        size_t part_body_len = (size_t)(next - part_body);

        /* Grow parts array. */
        if (mp->count >= mp->cap) {
            size_t new_cap = mp->cap ? mp->cap * 2 : 4;
            _http_multipart_part_t* tmp = realloc(
                mp->parts, new_cap * sizeof(*tmp));
            if (!tmp) {
                break;
            }
            mp->parts = tmp;
            mp->cap = new_cap;
        }

        _http_multipart_part_t* part = &mp->parts[mp->count];
        memset(part, 0, sizeof(*part));

        /* Extract name and filename from Content-Disposition. */
        /* Build a temporary NUL-terminated header string. */
        char* hdr_str = malloc(hdr_len + 1);
        if (hdr_str) {
            memcpy(hdr_str, p, hdr_len);
            hdr_str[hdr_len] = '\0';
            part->name     = _http_multipart_attr(hdr_str, "name");
            part->filename = _http_multipart_attr(hdr_str, "filename");
            part->content_type = _http_multipart_ct(hdr_str, hdr_len);
            free(hdr_str);
        }

        /* Copy part body. */
        part->data = malloc(part_body_len + 1);
        if (part->data) {
            memcpy(part->data, part_body, part_body_len);
            part->data[part_body_len] = '\0';
            part->data_len = part_body_len;
        }

        mp->count++;

        /* Advance past delimiter. */
        p = next + delim_len;
        /* Check for closing "--" or "\r\n". */
        if (p + 2 <= end && p[0] == '-' && p[1] == '-') {
            break; /* Final boundary. */
        }
        if (p + 2 <= end && p[0] == '\r' && p[1] == '\n') {
            p += 2;
        }
    }

    free(delim);

    if (mp->count == 0) {
        free(mp->parts);
        free(mp);
        return NULL;
    }

    return mp;
}

size_t xylem_http_multipart_count(const xylem_http_multipart_t* mp) {
    return mp ? mp->count : 0;
}

const char* xylem_http_multipart_name(
    const xylem_http_multipart_t* mp, size_t index) {
    if (!mp || index >= mp->count) {
        return NULL;
    }
    return mp->parts[index].name;
}

const char* xylem_http_multipart_filename(
    const xylem_http_multipart_t* mp, size_t index) {
    if (!mp || index >= mp->count) {
        return NULL;
    }
    return mp->parts[index].filename;
}

const char* xylem_http_multipart_content_type(
    const xylem_http_multipart_t* mp, size_t index) {
    if (!mp || index >= mp->count) {
        return NULL;
    }
    return mp->parts[index].content_type;
}

const void* xylem_http_multipart_data(
    const xylem_http_multipart_t* mp, size_t index) {
    if (!mp || index >= mp->count) {
        return NULL;
    }
    return mp->parts[index].data;
}

size_t xylem_http_multipart_data_len(
    const xylem_http_multipart_t* mp, size_t index) {
    if (!mp || index >= mp->count) {
        return 0;
    }
    return mp->parts[index].data_len;
}

void xylem_http_multipart_destroy(xylem_http_multipart_t* mp) {
    if (!mp) {
        return;
    }
    for (size_t i = 0; i < mp->count; i++) {
        free(mp->parts[i].name);
        free(mp->parts[i].filename);
        free(mp->parts[i].content_type);
        free(mp->parts[i].data);
    }
    free(mp->parts);
    free(mp);
}
