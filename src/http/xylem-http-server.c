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
#include "http-transport.h"
#include "llhttp.h"

#include "xylem/http/xylem-http-server.h"
#include "xylem/xylem-loop.h"

#include <stdlib.h>

struct xylem_http_req_s {
    char           method[16];
    char*          url;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
};

struct xylem_http_conn_s {
    xylem_http_srv_t*          srv;
    const http_transport_vt_t* vt;
    void*                      transport;
    llhttp_t                   parser;
    llhttp_settings_t          settings;
    xylem_http_req_t           req;
    xylem_loop_timer_t         idle_timer;
    bool                       keep_alive;
    bool                       closed;
    char*                      cur_header_name;
    size_t                     cur_header_name_len;
};

struct xylem_http_srv_s {
    xylem_loop_t*              loop;
    xylem_http_srv_cfg_t       cfg;
    const http_transport_vt_t* vt;
    void*                      listener;
    http_transport_cb_t        transport_cb;
    bool                       running;
};

const char* xylem_http_req_method(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    return req->method;
}

const char* xylem_http_req_url(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    return req->url;
}

const char* xylem_http_req_header(const xylem_http_req_t* req,
                                  const char* name) {
    if (!req || !name) {
        return NULL;
    }
    return http_header_find(req->headers, req->header_count, name);
}


const void* xylem_http_req_body(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    return req->body;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    if (!req) {
        return 0;
    }
    return req->body_len;
}

xylem_http_srv_t* xylem_http_srv_create(xylem_loop_t* loop,
                                        const xylem_http_srv_cfg_t* cfg) {
    if (!loop || !cfg) {
        return NULL;
    }

    xylem_http_srv_t* srv = calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }

    srv->loop = loop;
    srv->cfg  = *cfg;

    if (srv->cfg.max_body_size == 0) {
        srv->cfg.max_body_size = 1048576; /* 1 MiB */
    }
    if (srv->cfg.idle_timeout_ms == 0) {
        srv->cfg.idle_timeout_ms = 60000;
    }

    if (cfg->tls_cert && cfg->tls_key) {
        srv->vt = http_transport_tls();
        if (!srv->vt) {
            free(srv);
            return NULL;
        }
    } else {
        srv->vt = http_transport_tcp();
    }

    return srv;
}

void xylem_http_srv_destroy(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    if (srv->running) {
        xylem_http_srv_stop(srv);
    }
    free(srv);
}

int xylem_http_srv_start(xylem_http_srv_t* srv) {
    if (!srv || srv->running) {
        return -1;
    }
    /* Full implementation in Task 11 */
    return -1;
}

void xylem_http_srv_stop(xylem_http_srv_t* srv) {
    if (!srv || !srv->running) {
        return;
    }
    srv->running = false;
}
