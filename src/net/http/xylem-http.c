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

/**
 * HTTP public API + scheme dispatch.
 *
 * Defines the opaque public request/response wrappers and the accessor
 * surface over them, the client verb helpers, and the listen/shutdown
 * entry points. Wire-format mechanics live in the protocol engine
 * (http.c); the functions here are thin adapters that convert the public
 * handle to the engine's http_req_t / http_res_t via first-member address
 * equivalence (C 6.7.2.1), i.e. (http_res_t*)pub == &pub->internal.
 */

#include "xylem/net/http/xylem-http.h"

#include "http.h"
#include "http-utils.h"
#include "http-transport-tcp.h"
#include "http-transport-tls.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The public xylem_http_req_t / xylem_http_res_t are opaque wrappers
 * whose single member is the internal struct used by the engine.
 */
struct xylem_http_req_s {
    http_req_t internal;
};

struct xylem_http_res_s {
    http_res_t internal;
};

/**
 * The engine (http.c) hands user callbacks a public handle by casting
 * (xylem_http_res_t*)&res where res is an internal http_res_t, relying on
 * the wrapper's internal member sharing the struct's address. Enforce that
 * layout invariant here so any reordering fails at compile time rather than
 * silently corrupting access through the cast.
 */
_Static_assert(offsetof(struct xylem_http_req_s, internal) == 0,
               "http_req_t must remain the first member of xylem_http_req_s");
_Static_assert(offsetof(struct xylem_http_res_s, internal) == 0,
               "http_res_t must remain the first member of xylem_http_res_s");

const char* xylem_http_req_method(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_method");

    return req->internal.method;
}

const char* xylem_http_req_url(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_url");

    return req->internal.url;
}

const char* xylem_http_req_header(const xylem_http_req_t* req,
                                  const char* name) {
    if (!req || !name) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_header");

    return http_header_find(req->internal.headers, req->internal.header_count,
                            name);
}

int xylem_http_req_headers(const xylem_http_req_t* req,
                           const xylem_http_hdr_t** headers,
                           size_t* count) {
    if (!req || !headers || !count) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_headers");

    *headers = (const xylem_http_hdr_t*)req->internal.headers;
    *count   = req->internal.header_count;
    return 0;
}

const void* xylem_http_req_body(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_body");

    return (const void*)req->internal.body;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    if (!req) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_body_len");

    return req->internal.body_len;
}

int xylem_http_req_remote_addr(const xylem_http_req_t* req,
                               char* host, size_t host_len,
                               uint16_t* port) {
    if (!req) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_remote_addr");
    if (host && host_len > 0) {
        snprintf(host, host_len, "%s", req->internal.remote_host);
    }
    if (port) {
        *port = req->internal.remote_port;
    }
    return 0;
}

int xylem_http_res_status(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_status");

    return res->internal.status_code;
}

const char* xylem_http_res_header(const xylem_http_res_t* res,
                                  const char* name) {
    if (!res || !name) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_header");

    return http_header_find(res->internal.headers, res->internal.header_count,
                            name);
}

const void* xylem_http_res_body(const xylem_http_res_t* res) {
    if (!res) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_body");

    return (const void*)res->internal.body;
}

size_t xylem_http_res_body_len(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_body_len");

    return res->internal.body_len;
}

void xylem_http_res_destroy(xylem_http_res_t* res) {
    if (!res) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_destroy");

    http_headers_free(res->internal.headers, res->internal.header_count);
    free(res->internal.body);
    free(res);
}

int xylem_http_res_set_status(xylem_http_res_t* res, int code) {
    if (!res || res->internal._headers_sent) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_set_status");

    res->internal.status_code = code;
    return 0;
}

int xylem_http_res_set_header(xylem_http_res_t* res,
                              const char* name, const char* value) {
    if (!res || !name || !value) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_set_header");

    http_res_t* internal = &res->internal;
    if (internal->_headers_sent) {
        return -1;
    }
    return http_header_add(&internal->headers, &internal->header_count,
                           &internal->header_cap, name, strlen(name),
                           value, strlen(value));
}

int xylem_http_res_write(xylem_http_res_t* res,
                         const void* data, size_t len) {
    if (!res) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_write");

    return http_res_write(&res->internal, data, len);
}

int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport) {
    if (!res || !transport) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_upgrade");

    return http_res_upgrade(&res->internal, transport);
}

int xylem_http_res_hijack(xylem_http_res_t* res, void** transport) {
    if (!res || !transport) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_hijack");

    return http_res_hijack(&res->internal, transport);
}

void xylem_http_serve_content(xylem_http_res_t* res,
                              xylem_http_req_t* req,
                              const void* data,
                              size_t data_len,
                              const char* content_type) {
    if (!res || !req || !data || data_len == 0 || !content_type) {
        if (res) {
            xylem_http_res_set_status(res, 500);
            xylem_http_res_write(res, "Internal Server Error", 21);
        }
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_serve_content");

    const uint8_t* bytes = (const uint8_t*)data;

    /* Compute ETag: FNV-1a hash of content + size. */
    char etag[32];
    {
        uint32_t hash = 0x811c9dc5u;
        for (size_t i = 0; i < data_len; i++) {
            hash ^= bytes[i];
            hash *= 0x01000193u;
        }
        snprintf(etag, sizeof(etag), "\"%08x-%zx\"", hash, data_len);
    }

    /* If-None-Match: return 304. */
    const char* inm = xylem_http_req_header(req, "If-None-Match");
    if (inm && strcmp(inm, etag) == 0) {
        xylem_http_res_set_status(res, 304);
        xylem_http_res_set_header(res, "ETag", etag);
        return;
    }

    const char* method = xylem_http_req_method(req);

    /* Range request. */
    const char* range_hdr = xylem_http_req_header(req, "Range");
    if (range_hdr && strncmp(range_hdr, "bytes=", 6) == 0 &&
        strcmp(method, "HEAD") != 0) {
        size_t total = data_len;
        size_t start = 0, end_pos = total - 1;
        const char* spec = range_hdr + 6;

        if (spec[0] == '-') {
            size_t suffix = (size_t)strtoul(spec + 1, NULL, 10);
            if (suffix > total) {
                suffix = total;
            }
            start = total - suffix;
        } else {
            start = (size_t)strtoul(spec, NULL, 10);
            const char* dash = strchr(spec, '-');
            if (dash && dash[1] != '\0') {
                end_pos = (size_t)strtoul(dash + 1, NULL, 10);
            }
        }

        if (start > end_pos || start >= total) {
            xylem_http_res_set_status(res, 416);
            char cr[80];
            snprintf(cr, sizeof(cr), "bytes */%zu", total);
            xylem_http_res_set_header(res, "Content-Range", cr);
            xylem_http_res_write(res, "Range Not Satisfiable", 21);
            return;
        }
        if (end_pos >= total) {
            end_pos = total - 1;
        }

        char cr[80];
        snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", start, end_pos, total);
        xylem_http_res_set_header(res, "Content-Range", cr);
        xylem_http_res_set_header(res, "Accept-Ranges", "bytes");
        xylem_http_res_set_header(res, "Content-Type", content_type);
        xylem_http_res_set_header(res, "ETag", etag);
        xylem_http_res_set_status(res, 206);
        xylem_http_res_write(res, bytes + start, end_pos - start + 1);
        return;
    }

    /* Normal 200 response. */
    xylem_http_res_set_header(res, "Accept-Ranges", "bytes");
    xylem_http_res_set_header(res, "Content-Type", content_type);
    xylem_http_res_set_header(res, "ETag", etag);
    xylem_http_res_set_status(res, 200);

    if (strcmp(method, "HEAD") != 0) {
        xylem_http_res_write(res, data, data_len);
    }
}

static bool _is_https(const char* url) {
    return url && url[0] == 'h' && url[1] == 't' && url[2] == 't'
        && url[3] == 'p' && url[4] == 's' && url[5] == ':';
}

xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {
    if (!handler && !(opts && opts->on_upgrade)) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_listen");

    if (opts && opts->tls) {
        return http_tls_listen(host, port, handler, userdata, opts);
    }
    return http_tcp_listen(host, port, handler, userdata, opts);
}

void xylem_http_close(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_close");

    http_srv_t* s = (http_srv_t*)srv;
    /**
     * Stop accepting and signal in-flight connections to wind down, then
     * drop the owner reference. The accept coroutine and any live
     * connection coroutines each hold their own reference; whichever party
     * releases the last one frees `s`, so this never frees the server out
     * from under a coroutine still touching it. Returns without blocking.
     */
    atomic_store_explicit(&s->closing, true, memory_order_release);
    s->close_listener(s->listener);
    http_srv_unref(s);
}

int xylem_http_shutdown(xylem_http_srv_t* srv, uint64_t timeout_ms) {
    if (!srv) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_shutdown");

    http_srv_t* s = (http_srv_t*)srv;
    atomic_store_explicit(&s->closing, true, memory_order_release);
    s->close_listener(s->listener);

    if (timeout_ms == 0) {
        http_srv_unref(s);
        return 0;
    }

    /**
     * Wait until only the owner reference remains (every connection and the
     * accept coroutine have released theirs), bounded by the deadline. On
     * timeout we still drop the owner reference: any straggler coroutine
     * frees `s` when it finally exits, so there is no race either way.
     */
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    int rc = 0;
    while (atomic_load_explicit(&s->active_conns, memory_order_acquire) > 1) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            rc = -1;
            break;
        }
        runtime_sleep(1);
    }
    http_srv_unref(s);
    return rc;
}

int xylem_http_srv_addr(xylem_http_srv_t* srv,
                        char* host, size_t host_len,
                        uint16_t* port) {
    if (!srv) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_srv_addr");

    http_srv_t* s = (http_srv_t*)srv;
    if (host && host_len > 0) {
        snprintf(host, host_len, "%s", s->host);
    }
    if (port) {
        *port = s->port;
    }
    return 0;
}

static xylem_http_res_t* _do_request(const char* method, const char* url,
                                     const void* body, size_t body_len,
                                     const char* content_type,
                                     const xylem_http_hdr_t* headers,
                                     size_t header_count,
                                     const xylem_http_cli_opts_t* opts) {
    if (_is_https(url)) {
        return http_tls_request(method, url, body, body_len, content_type,
                                headers, header_count, opts);
    }
    return http_tcp_request(method, url, body, body_len, content_type,
                            headers, header_count, opts);
}

xylem_http_res_t* xylem_http_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts,
    xylem_http_cookie_jar_t*     jar) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_request");

    (void)jar;
    return _do_request(method, url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count,
                                 const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_get");

    return _do_request("GET", url, NULL, 0, NULL, headers, header_count, opts);
}

xylem_http_res_t* xylem_http_head(const char* url,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count,
                                  const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_head");

    return _do_request("HEAD", url, NULL, 0, NULL, headers, header_count, opts);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count,
                                  const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_post");

    return _do_request("POST", url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count,
                                 const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_put");

    return _do_request("PUT", url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_hdr_t* headers,
                                    size_t header_count,
                                    const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_delete");

    return _do_request("DELETE", url, NULL, 0, NULL,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_hdr_t* headers,
                                   size_t header_count,
                                   const xylem_http_cli_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_patch");

    return _do_request("PATCH", url, body, body_len, content_type,
                       headers, header_count, opts);
}
