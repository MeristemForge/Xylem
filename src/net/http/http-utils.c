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

#include "http-utils.h"

#include "xylem/encoding/xylem-base64.h"
#include "xylem/xylem-utils.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http_hex_digit(char c) {
    static const int8_t _hex_table[256] = {
        ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,
        ['4'] = 4,  ['5'] = 5,  ['6'] = 6,  ['7'] = 7,
        ['8'] = 8,  ['9'] = 9,
        ['A'] = 10, ['B'] = 11, ['C'] = 12,
        ['D'] = 13, ['E'] = 14, ['F'] = 15,
        ['a'] = 10, ['b'] = 11, ['c'] = 12,
        ['d'] = 13, ['e'] = 14, ['f'] = 15,
    };
    /* Table entries default to 0; distinguish '0' from invalid via range check. */
    uint8_t u = (uint8_t)c;
    int8_t v = _hex_table[u];
    if (v != 0) {
        return v;
    }
    return (c == '0') ? 0 : -1;
}


const uint8_t http_lower_table[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    91,92,93,94,95,96,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
    208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,
};


int http_url_parse(const char* url, http_url_t* out) {
    if (!url || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    const char* sep = strstr(url, "://");
    if (!sep) {
        return -1;
    }

    size_t scheme_len = (size_t)(sep - url);
    /* Only "http" (4) and "https" (5) are valid. */
    if (scheme_len != 4 && scheme_len != 5) {
        return -1;
    }

    for (size_t i = 0; i < scheme_len; i++) {
        out->scheme[i] = (char)http_lower_table[(uint8_t)url[i]];
    }
    out->scheme[scheme_len] = '\0';

    if (memcmp(out->scheme, "http", 4) != 0 ||
        (scheme_len == 5 && out->scheme[4] != 's')) {
        return -1;
    }

    const char* p = sep + 3;

    if (*p == '\0' || *p == '/' || *p == ':') {
        return -1;
    }

    const char* host_start = p;
    const char* host_end;

    if (*p == '[') {
        const char* bracket = strchr(p, ']');
        if (!bracket) {
            return -1;
        }
        host_start = p + 1;
        host_end = bracket;
        p = bracket + 1;
    } else {
        host_end = p;
        while (*host_end && *host_end != ':' && *host_end != '/') {
            host_end++;
        }
        p = host_end;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (*p == ':') {
        p++;
        char* end;
        long port_val = strtol(p, &end, 10);
        if (end == p || port_val <= 0 || port_val > 65535) {
            return -1;
        }
        out->port = (uint16_t)port_val;
        p = end;
    } else {
        out->port = (strcmp(out->scheme, "https") == 0) ? 443 : 80;
    }

    if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= sizeof(out->path)) {
            return -1;
        }
        memcpy(out->path, p, path_len);
        out->path[path_len] = '\0';
    } else if (*p == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        return -1;
    }

    return 0;
}

int http_url_serialize(const http_url_t* url, char* buf, size_t buf_size) {
    if (!url || !buf || buf_size == 0) {
        return -1;
    }

    bool is_default_port =
        (strcmp(url->scheme, "http") == 0 && url->port == 80) ||
        (strcmp(url->scheme, "https") == 0 && url->port == 443);

    int written;
    if (is_default_port) {
        written = snprintf(buf, buf_size, "%s://%s%s",
                           url->scheme, url->host, url->path);
    } else {
        written = snprintf(buf, buf_size, "%s://%s:%" PRIu16 "%s",
                           url->scheme, url->host, url->port, url->path);
    }

    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }
    return 0;
}


/* RFC 3986 unreserved characters: A-Z a-z 0-9 - . _ ~ */
static const uint8_t _unreserved_table[256] = {
    ['-'] = 1, ['.'] = 1, ['_'] = 1, ['~'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
    ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1,
    ['F'] = 1, ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1,
    ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1,
    ['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1,
    ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1,
    ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1,
    ['f'] = 1, ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1,
    ['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1, ['o'] = 1,
    ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1,
    ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1,
};

bool http_is_unreserved(uint8_t c) {
    return _unreserved_table[c] != 0;
}


char* http_req_serialize(const char* method, const http_url_t* url,
                         const void* body, size_t body_len,
                         const char* content_type, bool expect_continue,
                         size_t* out_len,
                         const xylem_http_hdr_t* custom_headers,
                         size_t custom_header_count,
                         const xylem_http_auth_t* auth) {
    char host_val[280];
    size_t host_val_len;
    bool is_default_port =
        (strcmp(url->scheme, "http") == 0 && url->port == 80) ||
        (strcmp(url->scheme, "https") == 0 && url->port == 443);

    if (is_default_port) {
        host_val_len = strlen(url->host);
        memcpy(host_val, url->host, host_val_len);
        host_val[host_val_len] = '\0';
    } else {
        host_val_len = (size_t)snprintf(host_val, sizeof(host_val),
                                        "%s:%" PRIu16,
                                        url->host, url->port);
    }

    const char* check_names[] = {
        "Host", "Content-Length", "Content-Type", "Connection", "Expect",
        "Authorization"
    };
    bool overridden[6];
    size_t custom_est = http_header_scan(custom_headers, custom_header_count,
                                         check_names, overridden, 6);
    bool host_overridden           = overridden[0];
    bool content_length_overridden = overridden[1];
    bool content_type_overridden   = overridden[2];
    bool connection_overridden     = overridden[3];
    bool expect_overridden         = overridden[4];
    bool auth_overridden           = overridden[5];

    /* Compute auth header if needed. */
    uint8_t* auth_b64 = NULL;
    int auth_b64_len = 0;
    if (!auth_overridden && auth && auth->username && auth->password) {
        size_t ulen = strlen(auth->username);
        size_t plen = strlen(auth->password);
        size_t cred_len = ulen + 1 + plen;
        char* cred = (char*)malloc(cred_len + 1);
        if (cred) {
            memcpy(cred, auth->username, ulen);
            cred[ulen] = ':';
            memcpy(cred + ulen + 1, auth->password, plen);
            cred[cred_len] = '\0';

            int b64_size = xylem_base64_encode_size((int)cred_len);
            auth_b64 = (uint8_t*)malloc((size_t)b64_size + 1);
            if (auth_b64) {
                auth_b64_len = xylem_base64_encode_std(
                    (const uint8_t*)cred, (int)cred_len,
                    auth_b64, b64_size + 1);
                if (auth_b64_len < 0) {
                    free(auth_b64);
                    auth_b64 = NULL;
                    auth_b64_len = 0;
                }
            }
            free(cred);
        }
    }

    size_t est = strlen(method) + 1 + strlen(url->path) + 11  /* request line */
               + custom_est
               + 6 + host_val_len + 2                          /* Host */
               + 16 + 20 + 2                                  /* Content-Length */
               + 24                                            /* Connection */
               + 2;                                            /* final CRLF */

    if (content_type) {
        est += 14 + strlen(content_type) + 2;  /* "Content-Type: " + value + "\r\n" */
    }
    if (auth_b64_len > 0) {
        est += 21 + (size_t)auth_b64_len + 2;  /* "Authorization: Basic " + b64 + "\r\n" */
    }
    if (expect_continue) {
        est += 24 + 2;  /* "Expect: 100-continue\r\n" */
    }
    if (!expect_continue) {
        est += body_len;
    }

    char* buf = (char*)malloc(est);
    if (!buf) {
        free(auth_b64);
        return NULL;
    }

    size_t off = 0;

    size_t method_len = strlen(method);
    size_t path_len = strlen(url->path);
    memcpy(buf + off, method, method_len);
    off += method_len;
    buf[off++] = ' ';
    memcpy(buf + off, url->path, path_len);
    off += path_len;
    memcpy(buf + off, " HTTP/1.1\r\n", 11);
    off += 11;

    for (size_t i = 0; i < custom_header_count; i++) {
        if (!custom_headers[i].name || !custom_headers[i].value) {
            continue;
        }
        size_t nlen = strlen(custom_headers[i].name);
        size_t vlen = strlen(custom_headers[i].value);
        memcpy(buf + off, custom_headers[i].name, nlen);
        off += nlen;
        buf[off++] = ':';
        buf[off++] = ' ';
        memcpy(buf + off, custom_headers[i].value, vlen);
        off += vlen;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    if (!host_overridden) {
        memcpy(buf + off, "Host: ", 6);
        off += 6;
        memcpy(buf + off, host_val, host_val_len);
        off += host_val_len;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    if (!content_length_overridden) {
        if (body_len > 0 || strcmp(method, "POST") == 0 ||
            strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) {
            memcpy(buf + off, "Content-Length: ", 16);
            off += 16;
            off += http_write_uint(buf + off, body_len);
            buf[off++] = '\r';
            buf[off++] = '\n';
        }
    }

    if (!content_type_overridden && content_type) {
        memcpy(buf + off, "Content-Type: ", 14);
        off += 14;
        size_t ctlen = strlen(content_type);
        memcpy(buf + off, content_type, ctlen);
        off += ctlen;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    if (!connection_overridden) {
        memcpy(buf + off, "Connection: keep-alive\r\n", 24);
        off += 24;
    }

    if (auth_b64_len > 0) {
        memcpy(buf + off, "Authorization: Basic ", 21);
        off += 21;
        memcpy(buf + off, auth_b64, (size_t)auth_b64_len);
        off += (size_t)auth_b64_len;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }
    free(auth_b64);

    if (!expect_overridden && expect_continue) {
        memcpy(buf + off, "Expect: 100-continue\r\n", 22);
        off += 22;
    }

    buf[off++] = '\r';
    buf[off++] = '\n';

    if (!expect_continue && body && body_len > 0) {
        memcpy(buf + off, body, body_len);
        off += body_len;
    }

    if (out_len) {
        *out_len = off;
    }
    return buf;
}

size_t http_write_uint(char* buf, size_t val) {
    /* Max 20 digits for 64-bit size_t. Write digits in reverse, then flip. */
    char tmp[20];
    size_t n = 0;
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    while (val > 0) {
        tmp[n++] = (char)('0' + val % 10);
        val /= 10;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

bool http_header_eq(const char* a, const char* b) {
    /* Fast path: if lengths differ, strings can't be equal. */
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) {
        return false;
    }
    for (size_t i = 0; i < la; i++) {
        if (http_lower_table[(uint8_t)a[i]] != http_lower_table[(uint8_t)b[i]]) {
            return false;
        }
    }
    return true;
}

const char* http_header_find(const http_header_t* headers,
                             size_t count, const char* name) {
    for (size_t i = 0; i < count; i++) {
        if (http_header_eq(headers[i].name, name)) {
            return headers[i].value;
        }
    }
    return NULL;
}

int http_header_add(http_header_t** headers, size_t* count,
                    size_t* cap, const char* name, size_t name_len,
                    const char* value, size_t value_len) {
    if (*count >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : *cap * 2;
        http_header_t* tmp = (http_header_t*)realloc(
            *headers, new_cap * sizeof(http_header_t));
        if (!tmp) {
            return -1;
        }
        *headers = tmp;
        *cap = new_cap;
    }

    /* Single allocation for both name and value strings. */
    char* block = (char*)malloc(name_len + 1 + value_len + 1);
    if (!block) {
        return -1;
    }
    memcpy(block, name, name_len);
    block[name_len] = '\0';
    char* v = block + name_len + 1;
    memcpy(v, value, value_len);
    v[value_len] = '\0';

    (*headers)[*count].name  = block;
    (*headers)[*count].value = v;
    (*count)++;
    return 0;
}

void http_headers_free(http_header_t* headers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        /* name and value share a single allocation; value is inside the block. */
        free(headers[i].name);
    }
    free(headers);
}


size_t http_header_scan(const xylem_http_hdr_t* headers, size_t count,
                        const char** check_names, bool* overridden,
                        size_t check_count) {
    size_t found = 0;
    for (size_t j = 0; j < check_count; j++) {
        overridden[j] = false;
    }

    size_t est = 0;
    for (size_t i = 0; i < count; i++) {
        if (!headers[i].name || !headers[i].value) {
            continue;
        }
        est += strlen(headers[i].name) + 2
             + strlen(headers[i].value) + 2;

        /* All check names already matched -- skip inner loop. */
        if (found >= check_count) {
            continue;
        }
        for (size_t j = 0; j < check_count; j++) {
            if (overridden[j]) {
                continue;
            }
            if (http_header_eq(headers[i].name, check_names[j])) {
                overridden[j] = true;
                found++;
                break;
            }
        }
    }
    return est;
}

const char* http_reason_phrase(int status) {
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "";
    }
}



char* xylem_http_url_encode(const char* src, size_t src_len,
                            size_t* out_len) {
    if (!src && src_len > 0) {
        return NULL;
    }

    size_t max_len = src_len * 3 + 1;
    char* out = (char*)malloc(max_len);
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

    char* out = (char*)malloc(src_len + 1);
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
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char* start = p;
        while (*p && *p != ',') {
            p++;
        }
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

    if (n < out_cap) {
        out[n].name = "Access-Control-Allow-Origin";
        if (cors->allow_credentials) {
            /* Spec requires echoing actual origin when credentials are allowed. */
            out[n].value = origin;
        } else {
            out[n].value = cors->allowed_origins;
        }
        n++;
    }

    if (cors->allow_credentials && n < out_cap) {
        out[n].name  = "Access-Control-Allow-Credentials";
        out[n].value = "true";
        n++;
    }

    if (cors->expose_headers && !is_preflight && n < out_cap) {
        out[n].name  = "Access-Control-Expose-Headers";
        out[n].value = cors->expose_headers;
        n++;
    }

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
            /* Static buffer: caller must consume headers before the next call. */
            static char _cors_max_age_buf[16];
            snprintf(_cors_max_age_buf, sizeof(_cors_max_age_buf),
                     "%d", cors->max_age);
            out[n].name  = "Access-Control-Max-Age";
            out[n].value = _cors_max_age_buf;
            n++;
        }
    }

    /* Non-wildcard origins require Vary: Origin for correct cache keying. */
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
        /* Guard against substring matches (e.g. "filename" vs "name"). */
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
        char* val = (char*)malloc(len + 1);
        if (!val) {
            return NULL;
        }
        memcpy(val, start, len);
        val[len] = '\0';
        return val;
    }
    return NULL;
}

static char* _http_multipart_ct(const char* headers, size_t hdr_len) {
    const char* p = headers;
    const char* end = headers + hdr_len;

    while (p < end) {
        const char* line = p;
        const char* line_end = line;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        size_t line_len = (size_t)(line_end - line);

        if (line_len > 13) {
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
                char* ct = (char*)malloc(remain + 1);
                if (!ct) {
                    return NULL;
                }
                memcpy(ct, v, remain);
                ct[remain] = '\0';
                return ct;
            }
        }

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

    size_t delim_len = 4 + bnd_len;
    char* delim = (char*)malloc(delim_len + 1);
    if (!delim) {
        return NULL;
    }
    memcpy(delim, "\r\n--", 4);
    memcpy(delim + 4, bnd, bnd_len);
    delim[delim_len] = '\0';

    xylem_http_multipart_t* mp =
        (xylem_http_multipart_t*)calloc(1, sizeof(xylem_http_multipart_t));
    if (!mp) {
        free(delim);
        return NULL;
    }

    const char* data = (const char*)body;
    const char* end  = data + body_len;

    /* First boundary has no leading \r\n (RFC 2046). */
    const char* p = data;
    if (body_len < 2 + bnd_len || memcmp(p, "--", 2) != 0 ||
        memcmp(p + 2, bnd, bnd_len) != 0) {
        free(delim);
        free(mp);
        return NULL;
    }
    p += 2 + bnd_len;
    if (p + 2 <= end && p[0] == '\r' && p[1] == '\n') {
        p += 2;
    }

    while (p < end) {
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

        if (mp->count >= mp->cap) {
            size_t new_cap = mp->cap ? mp->cap * 2 : 4;
            _http_multipart_part_t* tmp = (_http_multipart_part_t*)realloc(
                mp->parts, new_cap * sizeof(*tmp));
            if (!tmp) {
                break;
            }
            mp->parts = tmp;
            mp->cap = new_cap;
        }

        _http_multipart_part_t* part = &mp->parts[mp->count];
        memset(part, 0, sizeof(*part));

        char* hdr_str = (char*)malloc(hdr_len + 1);
        if (hdr_str) {
            memcpy(hdr_str, p, hdr_len);
            hdr_str[hdr_len] = '\0';
            part->name     = _http_multipart_attr(hdr_str, "name");
            part->filename = _http_multipart_attr(hdr_str, "filename");
            part->content_type = _http_multipart_ct(hdr_str, hdr_len);
            free(hdr_str);
        }

        part->data = (uint8_t*)malloc(part_body_len + 1);
        if (part->data) {
            memcpy(part->data, part_body, part_body_len);
            part->data[part_body_len] = '\0';
            part->data_len = part_body_len;
        }

        mp->count++;

        p = next + delim_len;
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


typedef struct {
    char*    name;
    char*    filename;
    char*    content_type;
    uint8_t* data;
    size_t   data_len;
} _build_part_t;

struct xylem_http_multipart_builder_s {
    _build_part_t* parts;
    size_t         count;
    size_t         cap;
    char           boundary[25];
};

/* Generate a 24-char alphanumeric boundary using nanosecond time as seed. */
static void _build_gen_boundary(char* out) {
    static const char _alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static uint64_t _counter = 0;
    uint64_t seed = xylem_utils_getnow(XYLEM_TIME_PRECISION_NSEC) ^ (++_counter);
    for (int i = 0; i < 24; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = _alphabet[(seed >> 33) % 62];
    }
    out[24] = '\0';
}

xylem_http_multipart_builder_t* xylem_http_multipart_build_create(void) {
    xylem_http_multipart_builder_t* b =
        (xylem_http_multipart_builder_t*)calloc(
            1, sizeof(xylem_http_multipart_builder_t));
    if (!b) {
        return NULL;
    }
    _build_gen_boundary(b->boundary);
    return b;
}

/* Grow parts array and append a new part. */
static int _build_add_part(xylem_http_multipart_builder_t* b,
                           const char* name,
                           const char* filename,
                           const char* content_type,
                           const void* data,
                           size_t data_len) {
    if (!b || !name) {
        return -1;
    }

    if (b->count >= b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4;
        _build_part_t* tmp = (_build_part_t*)realloc(
            b->parts, new_cap * sizeof(_build_part_t));
        if (!tmp) {
            return -1;
        }
        b->parts = tmp;
        b->cap = new_cap;
    }

    _build_part_t* part = &b->parts[b->count];
    memset(part, 0, sizeof(*part));

    size_t name_len = strlen(name);
    part->name = (char*)malloc(name_len + 1);
    if (!part->name) {
        return -1;
    }
    memcpy(part->name, name, name_len + 1);

    if (filename) {
        size_t fn_len = strlen(filename);
        part->filename = (char*)malloc(fn_len + 1);
        if (!part->filename) {
            free(part->name);
            part->name = NULL;
            return -1;
        }
        memcpy(part->filename, filename, fn_len + 1);
    }

    if (content_type) {
        size_t ct_len = strlen(content_type);
        part->content_type = (char*)malloc(ct_len + 1);
        if (!part->content_type) {
            free(part->name);
            free(part->filename);
            part->name = NULL;
            part->filename = NULL;
            return -1;
        }
        memcpy(part->content_type, content_type, ct_len + 1);
    }

    if (data && data_len > 0) {
        part->data = (uint8_t*)malloc(data_len);
        if (!part->data) {
            free(part->name);
            free(part->filename);
            free(part->content_type);
            part->name = NULL;
            part->filename = NULL;
            part->content_type = NULL;
            return -1;
        }
        memcpy(part->data, data, data_len);
        part->data_len = data_len;
    }

    b->count++;
    return 0;
}

int xylem_http_multipart_build_field(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const void* value,
    size_t value_len) {
    return _build_add_part(b, name, NULL, NULL, value, value_len);
}

int xylem_http_multipart_build_file(
    xylem_http_multipart_builder_t* b,
    const char* name,
    const char* filename,
    const char* content_type,
    const void* data,
    size_t data_len) {
    if (!filename) {
        return -1;
    }
    if (!content_type) {
        content_type = "application/octet-stream";
    }
    return _build_add_part(b, name, filename, content_type, data, data_len);
}

int xylem_http_multipart_build_finish(
    xylem_http_multipart_builder_t* b,
    void** body,
    size_t* body_len,
    char** content_type) {
    if (!b || !body || !body_len || !content_type) {
        return -1;
    }
    if (b->count == 0) {
        return -1;
    }

    size_t bnd_len = strlen(b->boundary);

    /* Estimate total size. */
    size_t total = 0;
    for (size_t i = 0; i < b->count; i++) {
        _build_part_t* p = &b->parts[i];
        /* "--" + boundary + "\r\n" */
        total += 2 + bnd_len + 2;
        /* Content-Disposition: form-data; name="<name>" */
        total += 38 + strlen(p->name) + 1;
        if (p->filename) {
            /* ; filename="<filename>" */
            total += 12 + strlen(p->filename) + 1;
        }
        total += 2; /* \r\n after Content-Disposition */
        if (p->content_type) {
            /* Content-Type: <ct>\r\n */
            total += 14 + strlen(p->content_type) + 2;
        }
        total += 2; /* blank line \r\n */
        total += p->data_len;
        total += 2; /* \r\n after data */
    }
    /* Final boundary: "--" + boundary + "--\r\n" */
    total += 2 + bnd_len + 4;

    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) {
        return -1;
    }

    size_t off = 0;
    for (size_t i = 0; i < b->count; i++) {
        _build_part_t* p = &b->parts[i];

        /* Boundary delimiter. */
        buf[off++] = '-';
        buf[off++] = '-';
        memcpy(buf + off, b->boundary, bnd_len);
        off += bnd_len;
        buf[off++] = '\r';
        buf[off++] = '\n';

        /* Content-Disposition header. */
        int n;
        if (p->filename) {
            n = snprintf(
                (char*)(buf + off), total - off,
                "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"",
                p->name, p->filename);
        } else {
            n = snprintf(
                (char*)(buf + off), total - off,
                "Content-Disposition: form-data; name=\"%s\"",
                p->name);
        }
        if (n < 0 || (size_t)n >= total - off) {
            free(buf);
            return -1;
        }
        off += (size_t)n;
        buf[off++] = '\r';
        buf[off++] = '\n';

        /* Content-Type header (file parts only). */
        if (p->content_type) {
            n = snprintf(
                (char*)(buf + off), total - off,
                "Content-Type: %s", p->content_type);
            if (n < 0 || (size_t)n >= total - off) {
                free(buf);
                return -1;
            }
            off += (size_t)n;
            buf[off++] = '\r';
            buf[off++] = '\n';
        }

        /* Blank line separating headers from body. */
        buf[off++] = '\r';
        buf[off++] = '\n';

        /* Part data. */
        if (p->data_len > 0) {
            memcpy(buf + off, p->data, p->data_len);
            off += p->data_len;
        }

        /* Trailing \r\n after data. */
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    /* Final boundary. */
    buf[off++] = '-';
    buf[off++] = '-';
    memcpy(buf + off, b->boundary, bnd_len);
    off += bnd_len;
    buf[off++] = '-';
    buf[off++] = '-';
    buf[off++] = '\r';
    buf[off++] = '\n';

    *body = buf;
    *body_len = off;

    /* Build content_type string: "multipart/form-data; boundary=<boundary>" */
    size_t ct_len = 30 + bnd_len; /* "multipart/form-data; boundary=" + boundary */
    char* ct = (char*)malloc(ct_len + 1);
    if (!ct) {
        free(buf);
        *body = NULL;
        *body_len = 0;
        return -1;
    }
    snprintf(ct, ct_len + 1, "multipart/form-data; boundary=%s", b->boundary);
    *content_type = ct;

    return 0;
}

void xylem_http_multipart_build_destroy(
    xylem_http_multipart_builder_t* b) {
    if (!b) {
        return;
    }
    for (size_t i = 0; i < b->count; i++) {
        free(b->parts[i].name);
        free(b->parts[i].filename);
        free(b->parts[i].content_type);
        free(b->parts[i].data);
    }
    free(b->parts);
    free(b);
}
