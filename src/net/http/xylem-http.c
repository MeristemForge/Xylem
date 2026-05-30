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

#include "xylem/net/http/xylem-http.h"

#include "http-common.h"
#include "runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (opts && opts->tls) {
        return http_listen_tls(host, port, handler, userdata, opts);
    }
    return http_listen_tcp(host, port, handler, userdata, opts);
}

void xylem_http_close(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    http_srv_t* s = (http_srv_t*)srv;
    s->close_listener(s->listener);
    free(s);
}

int xylem_http_shutdown(xylem_http_srv_t* srv, uint64_t timeout_ms) {
    if (!srv) {
        return -1;
    }
    http_srv_t* s = (http_srv_t*)srv;
    s->closing = true;
    s->close_listener(s->listener);

    if (timeout_ms == 0) {
        free(s);
        return 0;
    }

    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    while (atomic_load(&s->active_conns) > 0) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            free(s);
            return -1;
        }
        runtime_sleep(1);
    }
    free(s);
    return 0;
}

int xylem_http_srv_addr(xylem_http_srv_t* srv,
                        char* host, size_t host_len,
                        uint16_t* port) {
    if (!srv) {
        return -1;
    }
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
        return http_request_tls(method, url, body, body_len, content_type,
                                headers, header_count, opts);
    }
    return http_request_tcp(method, url, body, body_len, content_type,
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
    (void)jar;
    return _do_request(method, url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count,
                                 const xylem_http_cli_opts_t* opts) {
    return _do_request("GET", url, NULL, 0, NULL, headers, header_count, opts);
}

xylem_http_res_t* xylem_http_head(const char* url,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count,
                                  const xylem_http_cli_opts_t* opts) {
    return _do_request("HEAD", url, NULL, 0, NULL, headers, header_count, opts);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count,
                                  const xylem_http_cli_opts_t* opts) {
    return _do_request("POST", url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count,
                                 const xylem_http_cli_opts_t* opts) {
    return _do_request("PUT", url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_hdr_t* headers,
                                    size_t header_count,
                                    const xylem_http_cli_opts_t* opts) {
    return _do_request("DELETE", url, NULL, 0, NULL,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_hdr_t* headers,
                                   size_t header_count,
                                   const xylem_http_cli_opts_t* opts) {
    return _do_request("PATCH", url, body, body_len, content_type,
                       headers, header_count, opts);
}
