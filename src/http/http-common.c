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

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http_hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}


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
    if (scheme_len == 0 || scheme_len >= sizeof(out->scheme)) {
        return -1;
    }

    for (size_t i = 0; i < scheme_len; i++) {
        out->scheme[i] = (char)tolower((unsigned char)url[i]);
    }
    out->scheme[scheme_len] = '\0';

    if (strcmp(out->scheme, "http") != 0 &&
        strcmp(out->scheme, "https") != 0) {
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


bool http_is_unreserved(uint8_t c) {
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= '0' && c <= '9') {
        return true;
    }
    return c == '-' || c == '.' || c == '_' || c == '~';
}


/**
 * Serialize an HTTP/1.1 request into a malloc'd buffer.
 * Fills Host, Content-Length, Connection, Content-Type, Expect headers.
 * Returns the buffer and sets *out_len. Caller frees.
 * Returns NULL on failure.
 *
 * When expect_continue is true, the body is NOT appended -- the caller
 * sends headers first, waits for 100, then sends body separately.
 */
char* http_req_serialize(const char* method, const http_url_t* url,
                         const void* body, size_t body_len,
                         const char* content_type, bool expect_continue,
                         size_t* out_len) {
    char host_val[280];
    bool is_default_port =
        (strcmp(url->scheme, "http") == 0 && url->port == 80) ||
        (strcmp(url->scheme, "https") == 0 && url->port == 443);

    if (is_default_port) {
        snprintf(host_val, sizeof(host_val), "%s", url->host);
    } else {
        snprintf(host_val, sizeof(host_val), "%s:%" PRIu16,
                 url->host, url->port);
    }

    /**
     * Estimate buffer size:
     * request line + Host + Content-Length + Content-Type +
     * Connection + Expect + CRLF + body
     */
    size_t est = strlen(method) + 1 + strlen(url->path) + 11
               + 6 + strlen(host_val) + 2
               + 30
               + 24
               + 2;

    if (content_type) {
        est += 16 + strlen(content_type) + 2;
    }
    if (expect_continue) {
        est += 26;
    }
    if (!expect_continue) {
        est += body_len;
    }

    char* buf = malloc(est);
    if (!buf) {
        return NULL;
    }

    size_t off = 0;

    off += (size_t)snprintf(buf + off, est - off, "%s %s HTTP/1.1\r\n",
                            method, url->path);

    off += (size_t)snprintf(buf + off, est - off, "Host: %s\r\n", host_val);

    if (body_len > 0 || strcmp(method, "POST") == 0 ||
        strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) {
        off += (size_t)snprintf(buf + off, est - off,
                                "Content-Length: %zu\r\n", body_len);
    }

    if (content_type) {
        off += (size_t)snprintf(buf + off, est - off,
                                "Content-Type: %s\r\n", content_type);
    }

    off += (size_t)snprintf(buf + off, est - off,
                            "Connection: keep-alive\r\n");

    if (expect_continue) {
        off += (size_t)snprintf(buf + off, est - off,
                                "Expect: 100-continue\r\n");
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

bool http_header_eq(const char* a, const char* b) {
    for (;; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        if (*a == '\0') {
            return true;
        }
    }
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
        http_header_t* tmp = realloc(*headers,
                                     new_cap * sizeof(http_header_t));
        if (!tmp) {
            return -1;
        }
        *headers = tmp;
        *cap = new_cap;
    }

    char* n = malloc(name_len + 1);
    char* v = malloc(value_len + 1);
    if (!n || !v) {
        free(n);
        free(v);
        return -1;
    }
    memcpy(n, name, name_len);
    n[name_len] = '\0';
    memcpy(v, value, value_len);
    v[value_len] = '\0';

    (*headers)[*count].name  = n;
    (*headers)[*count].value = v;
    (*count)++;
    return 0;
}

void http_headers_free(http_header_t* headers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(headers[i].name);
        free(headers[i].value);
    }
    free(headers);
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
