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
 * Defines the public request/response accessor surface, the client verb
 * helpers, and the listen/shutdown entry points. Wire-format mechanics
 * live in the protocol engine (http.c).
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

const char* xylem_http_req_method(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_method");

    return req->method;
}

const char* xylem_http_req_url(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_url");

    return req->url;
}

const char* xylem_http_req_header(const xylem_http_req_t* req,
                                  const char* name) {
    if (!req || !name) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_header");

    return http_header_find(req->headers, req->header_count, name);
}

int xylem_http_req_headers(const xylem_http_req_t* req,
                           const xylem_http_hdr_t** headers,
                           size_t* count) {
    if (!req || !headers || !count) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_headers");

    *headers = (const xylem_http_hdr_t*)req->headers;
    *count   = req->header_count;
    return 0;
}

const void* xylem_http_req_body(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_body");

    return (const void*)req->body;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    if (!req) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_body_len");

    return req->body_len;
}

int xylem_http_req_remote_addr(const xylem_http_req_t* req,
                               char* host, size_t host_len,
                               uint16_t* port) {
    if (!req) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_req_remote_addr");
    if (host && host_len > 0) {
        snprintf(host, host_len, "%s", req->remote_host);
    }
    if (port) {
        *port = req->remote_port;
    }
    return 0;
}

int xylem_http_res_status(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_status");

    return res->status_code;
}

const char* xylem_http_res_header(const xylem_http_res_t* res,
                                  const char* name) {
    if (!res || !name) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_header");

    return http_header_find(res->headers, res->header_count, name);
}

const void* xylem_http_res_body(const xylem_http_res_t* res) {
    if (!res) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_body");

    return (const void*)res->body;
}

size_t xylem_http_res_body_len(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_body_len");

    return res->body_len;
}

void xylem_http_res_destroy(xylem_http_res_t* res) {
    if (!res) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_res_destroy");

    http_headers_free(res->headers, res->header_count);
    free(res->body);
    free(res);
}

int xylem_http_writer_set_status(xylem_http_writer_t* writer, int code) {
    if (!writer || writer->state != HTTP_WRITER_OPEN
        || writer->headers_sent) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_writer_set_status");

    writer->status_code = code;
    writer->started     = true;
    return 0;
}

int xylem_http_writer_set_header(
    xylem_http_writer_t* writer,
    const char*          name,
    const char*          value) {
    if (!writer || !name || !value || writer->state != HTTP_WRITER_OPEN) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_writer_set_header");

    if (writer->headers_sent) {
        return -1;
    }
    int rc = http_header_add(&writer->headers, &writer->header_count,
                             &writer->header_cap, name, strlen(name),
                             value, strlen(value));
    if (rc == 0) {
        writer->started = true;
    }
    return rc;
}

int xylem_http_writer_write(
    xylem_http_writer_t* writer,
    const void*          data,
    size_t               len) {
    if (!writer || writer->state != HTTP_WRITER_OPEN) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_writer_write");

    if (len == 0) {
        return 0;
    }
    if (!data || !writer->ops || !writer->ops->write) {
        return -1;
    }
    int rc = writer->ops->write(writer, data, len);
    if (rc == 0) {
        writer->started = true;
    }
    return rc;
}

int xylem_http_writer_flush(xylem_http_writer_t* writer) {
    if (!writer || writer->state != HTTP_WRITER_OPEN || !writer->ops
        || !writer->ops->flush) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_writer_flush");

    int rc = writer->ops->flush(writer);
    if (rc == 0) {
        writer->started = true;
    }
    return rc;
}

int xylem_http_writer_upgrade(
    xylem_http_writer_t* writer,
    void**               transport) {
    if (!writer || !transport || writer->state != HTTP_WRITER_OPEN
        || !writer->ops || !writer->ops->upgrade) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_writer_upgrade");

    int rc = writer->ops->upgrade(writer, transport);
    if (rc == 0) {
        writer->state        = HTTP_WRITER_HIJACKED;
        writer->started      = true;
        writer->headers_sent = true;
    } else {
        writer->state = HTTP_WRITER_ABORTED;
    }
    return rc;
}

int xylem_http_writer_hijack(
    xylem_http_writer_t* writer,
    void**               transport) {
    if (!writer || !transport || writer->state != HTTP_WRITER_OPEN
        || !writer->ops || !writer->ops->hijack) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_writer_hijack");

    int rc = writer->ops->hijack(writer, transport);
    if (rc == 0) {
        writer->state        = HTTP_WRITER_HIJACKED;
        writer->started      = true;
        writer->headers_sent = true;
    }
    return rc;
}

int http_writer_finish(http_writer_t* writer) {
    if (!writer || writer->state != HTTP_WRITER_OPEN || !writer->ops
        || !writer->ops->finish) {
        return -1;
    }
    if (writer->ops->finish(writer) != 0) {
        writer->state = HTTP_WRITER_ABORTED;
        return -1;
    }
    writer->state = HTTP_WRITER_FINISHED;
    return 0;
}

bool http_writer_started(const http_writer_t* writer) {
    return writer && writer->started;
}

void xylem_http_serve_content(
    xylem_http_writer_t* writer,
    xylem_http_req_t*    req,
    const void*          data,
    size_t               data_len,
    const char*          content_type) {
    if (!writer || !req || !data || data_len == 0 || !content_type) {
        if (writer) {
            xylem_http_writer_set_status(writer, 500);
            xylem_http_writer_write(writer, "Internal Server Error", 21);
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
        xylem_http_writer_set_status(writer, 304);
        xylem_http_writer_set_header(writer, "ETag", etag);
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
            xylem_http_writer_set_status(writer, 416);
            char cr[80];
            snprintf(cr, sizeof(cr), "bytes */%zu", total);
            xylem_http_writer_set_header(writer, "Content-Range", cr);
            xylem_http_writer_write(writer, "Range Not Satisfiable", 21);
            return;
        }
        if (end_pos >= total) {
            end_pos = total - 1;
        }

        char cr[80];
        snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", start, end_pos, total);
        xylem_http_writer_set_header(writer, "Content-Range", cr);
        xylem_http_writer_set_header(writer, "Accept-Ranges", "bytes");
        xylem_http_writer_set_header(writer, "Content-Type", content_type);
        xylem_http_writer_set_header(writer, "ETag", etag);
        xylem_http_writer_set_status(writer, 206);
        xylem_http_writer_write(writer, bytes + start, end_pos - start + 1);
        return;
    }

    /* Normal 200 response. */
    xylem_http_writer_set_header(writer, "Accept-Ranges", "bytes");
    xylem_http_writer_set_header(writer, "Content-Type", content_type);
    xylem_http_writer_set_header(writer, "ETag", etag);
    xylem_http_writer_set_status(writer, 200);

    if (strcmp(method, "HEAD") != 0) {
        xylem_http_writer_write(writer, data, data_len);
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

    /**
     * Stop accepting and signal in-flight connections to wind down, then
     * drop the owner reference. The accept coroutine and any live
     * connection coroutines each hold their own reference; whichever party
     * releases the last one frees `srv`, so this never frees the server out
     * from under a coroutine still touching it. Returns without blocking.
     */
    atomic_store(&srv->closing, true);
    srv->close_listener(srv->listener);
    http_srv_unref(srv);
}

int xylem_http_shutdown(xylem_http_srv_t* srv, uint64_t timeout_ms) {
    if (!srv) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_shutdown");

    atomic_store(&srv->closing, true);
    srv->close_listener(srv->listener);

    if (timeout_ms == 0) {
        http_srv_unref(srv);
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
    while (atomic_load(&srv->active_conns) > 1) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            rc = -1;
            break;
        }
        runtime_sleep(1);
    }
    http_srv_unref(srv);
    return rc;
}

int xylem_http_srv_addr(xylem_http_srv_t* srv,
                        char* host, size_t host_len,
                        uint16_t* port) {
    if (!srv) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("http", "xylem_http_srv_addr");

    if (host && host_len > 0) {
        snprintf(host, host_len, "%s", srv->host);
    }
    if (port) {
        *port = srv->port;
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
