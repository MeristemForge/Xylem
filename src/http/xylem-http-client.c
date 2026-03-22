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

#include "xylem/http/xylem-http-client.h"
#include "xylem/xylem-addr.h"
#include "xylem/xylem-loop.h"
#include "xylem/xylem-thrdpool.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct xylem_http_cli_res_s {
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
};

typedef struct {
    xylem_loop_t               loop;
    xylem_thrdpool_t*          pool;
    xylem_loop_timer_t         timeout_timer;
    http_url_t                 url;
    const char*                method;
    const void*                body;
    size_t                     body_len;
    const char*                content_type;
    xylem_http_cli_res_t*          res;
    llhttp_t                   parser;
    llhttp_settings_t          settings;
    char*                      cur_header_name;
    size_t                     cur_header_name_len;
    const http_transport_vt_t* vt;
    void*                      conn;
    http_transport_cb_t        transport_cb;
    bool                       timed_out;
    bool                       done;
    bool                       expect_continue;
    bool                       continue_received;
    int                        redirects_remaining;
} _http_client_ctx_t;

static _Thread_local uint64_t _http_timeout_ms    = 30000;
static _Thread_local int      _http_max_redirects  = 0;
static _Thread_local size_t   _http_max_body_size  = 10485760; /* 10 MiB */

/* Stop and close the timeout timer, then stop the event loop. */
static void _http_client_abort(_http_client_ctx_t* ctx) {
    if (ctx->timeout_timer.active) {
        xylem_loop_stop_timer(&ctx->timeout_timer);
    }
    xylem_loop_close_timer(&ctx->timeout_timer);
    xylem_loop_stop(&ctx->loop);
}

static int _http_res_header_field_cb(llhttp_t* parser,
                                     const char* at, size_t len) {
    _http_client_ctx_t* ctx = parser->data;

    free(ctx->cur_header_name);
    ctx->cur_header_name = malloc(len + 1);
    if (!ctx->cur_header_name) {
        return HPE_USER;
    }
    memcpy(ctx->cur_header_name, at, len);
    ctx->cur_header_name[len] = '\0';
    ctx->cur_header_name_len = len;
    return 0;
}

static int _http_res_header_value_cb(llhttp_t* parser,
                                     const char* at, size_t len) {
    _http_client_ctx_t* ctx = parser->data;
    if (!ctx->cur_header_name || !ctx->res) {
        return 0;
    }

    if (http_header_add(&ctx->res->headers, &ctx->res->header_count,
                        &ctx->res->header_cap,
                        ctx->cur_header_name, ctx->cur_header_name_len,
                        at, len) != 0) {
        return HPE_USER;
    }

    free(ctx->cur_header_name);
    ctx->cur_header_name = NULL;
    ctx->cur_header_name_len = 0;
    return 0;
}

static int _http_res_headers_complete_cb(llhttp_t* parser) {
    _http_client_ctx_t* ctx = parser->data;
    if (ctx->res) {
        ctx->res->status_code = (int)parser->status_code;
    }

    if (parser->status_code == 100) {
        ctx->continue_received = true;
    }

    return 0;
}

static int _http_res_body_cb(llhttp_t* parser,
                             const char* at, size_t len) {
    _http_client_ctx_t* ctx = parser->data;
    if (!ctx->res) {
        return 0;
    }

    if (ctx->res->body_len + len > _http_max_body_size) {
        return HPE_USER;
    }

    uint8_t* tmp = realloc(ctx->res->body, ctx->res->body_len + len);
    if (!tmp) {
        return HPE_USER;
    }
    memcpy(tmp + ctx->res->body_len, at, len);
    ctx->res->body = tmp;
    ctx->res->body_len += len;
    return 0;
}

static int _http_res_message_complete_cb(llhttp_t* parser) {
    _http_client_ctx_t* ctx = parser->data;
    ctx->done = true;
    return HPE_PAUSED;
}

void xylem_http_cli_set_timeout(uint64_t timeout_ms) {
    _http_timeout_ms = timeout_ms;
}

void xylem_http_cli_set_follow_redirects(int max_redirects) {
    _http_max_redirects = max_redirects;
}

void xylem_http_cli_set_max_body_size(size_t max_bytes) {
    _http_max_body_size = (max_bytes == 0) ? 10485760 : max_bytes;
}

int xylem_http_cli_res_status(const xylem_http_cli_res_t* res) {
    if (!res) {
        return 0;
    }
    return res->status_code;
}


const char* xylem_http_cli_res_header(const xylem_http_cli_res_t* res,
                                  const char* name) {
    if (!res || !name) {
        return NULL;
    }
    return http_header_find(res->headers, res->header_count, name);
}

const void* xylem_http_cli_res_body(const xylem_http_cli_res_t* res) {
    if (!res) {
        return NULL;
    }
    return res->body;
}

size_t xylem_http_cli_res_body_len(const xylem_http_cli_res_t* res) {
    if (!res) {
        return 0;
    }
    return res->body_len;
}

void xylem_http_cli_res_destroy(xylem_http_cli_res_t* res) {
    if (!res) {
        return;
    }
    http_headers_free(res->headers, res->header_count);
    free(res->body);
    free(res);
}

static void _http_client_timeout_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer) {
    (void)loop;
    _http_client_ctx_t* ctx =
        (_http_client_ctx_t*)((char*)timer -
            offsetof(_http_client_ctx_t, timeout_timer));
    ctx->timed_out = true;
    if (ctx->conn) {
        ctx->vt->close_conn(ctx->conn);
        ctx->conn = NULL;
    }
}

static void _http_client_read_cb(void* handle, void* user,
                                 void* data, size_t len) {
    (void)handle;
    _http_client_ctx_t* ctx = user;

    enum llhttp_errno err = llhttp_execute(&ctx->parser, data, len);

    if (ctx->done) {
        if (ctx->conn) {
            ctx->vt->close_conn(ctx->conn);
            ctx->conn = NULL;
        }
        return;
    }

    /**
     * 100 Continue received: send the body now.
     * Reset parser to continue reading the real response.
     */
    if (ctx->continue_received && ctx->expect_continue) {
        ctx->expect_continue = false;
        if (ctx->body && ctx->body_len > 0 && ctx->conn) {
            ctx->vt->send(ctx->conn, ctx->body, ctx->body_len);
        }
        llhttp_resume(&ctx->parser);
        return;
    }

    if (err != HPE_OK && err != HPE_PAUSED) {
        if (ctx->conn) {
            ctx->vt->close_conn(ctx->conn);
            ctx->conn = NULL;
        }
    }
}

static void _http_client_connect_cb(void* handle, void* user) {
    (void)handle;
    _http_client_ctx_t* ctx = user;

    bool use_continue = (ctx->body_len > 1024);
    ctx->expect_continue = use_continue;

    size_t req_len;
    char* req_buf = http_req_serialize(ctx->method, &ctx->url,
                                       ctx->body, ctx->body_len,
                                       ctx->content_type,
                                       use_continue, &req_len);
    if (!req_buf) {
        if (ctx->conn) {
            ctx->vt->close_conn(ctx->conn);
            ctx->conn = NULL;
        }
        return;
    }

    ctx->vt->send(ctx->conn, req_buf, req_len);
    free(req_buf);
}

static void _http_client_close_cb(void* handle, void* user, int err) {
    (void)handle;
    (void)err;
    _http_client_ctx_t* ctx = user;
    ctx->conn = NULL;
    _http_client_abort(ctx);
}

static void _http_client_resolve_cb(xylem_addr_t* addrs, size_t count,
                                    int status, void* userdata) {
    _http_client_ctx_t* ctx = userdata;

    if (status != 0 || count == 0) {
        _http_client_abort(ctx);
        return;
    }

    if (strcmp(ctx->url.scheme, "https") == 0) {
        ctx->vt = http_transport_tls();
        if (!ctx->vt) {
            _http_client_abort(ctx);
            return;
        }
    } else {
        ctx->vt = http_transport_tcp();
    }

    ctx->transport_cb.on_connect    = _http_client_connect_cb;
    ctx->transport_cb.on_read       = _http_client_read_cb;
    ctx->transport_cb.on_close      = _http_client_close_cb;
    ctx->transport_cb.on_write_done = NULL;
    ctx->transport_cb.on_accept     = NULL;

    ctx->conn = ctx->vt->dial(&ctx->loop, &addrs[0],
                              &ctx->transport_cb, ctx, NULL);
    if (!ctx->conn) {
        _http_client_abort(ctx);
    }
}


/**
 * Check whether the response is a redirect that should be followed.
 * On success, updates ctx->url and ctx->method for the next iteration,
 * destroys the current response, and returns true.
 */
static bool _http_client_follow_redirect(_http_client_ctx_t* ctx) {
    int status = ctx->res->status_code;
    if (ctx->redirects_remaining <= 0 ||
        (status != 301 && status != 302 &&
         status != 307 && status != 308)) {
        return false;
    }

    const char* location = http_header_find(
        ctx->res->headers, ctx->res->header_count, "Location");
    if (!location) {
        return false;
    }

    http_url_t new_url;
    int parse_rc;

    if (strstr(location, "://")) {
        parse_rc = http_url_parse(location, &new_url);
    } else {
        /**
         * Relative redirect: keep scheme/host/port,
         * replace path.
         */
        new_url = ctx->url;
        size_t loc_len = strlen(location);
        if (loc_len >= sizeof(new_url.path)) {
            parse_rc = -1;
        } else {
            memcpy(new_url.path, location, loc_len);
            new_url.path[loc_len] = '\0';
            parse_rc = 0;
        }
    }

    if (parse_rc != 0) {
        return false;
    }

    xylem_http_cli_res_destroy(ctx->res);
    ctx->res = NULL;
    ctx->url = new_url;
    ctx->redirects_remaining--;

    if (status == 301 || status == 302) {
        ctx->method       = "GET";
        ctx->body         = NULL;
        ctx->body_len     = 0;
        ctx->content_type = NULL;
    }

    return true;
}

static xylem_http_cli_res_t* _http_client_exec(const char* method,
                                           const char* url,
                                           const void* body,
                                           size_t body_len,
                                           const char* content_type) {
    if (!method || !url) {
        return NULL;
    }

    _http_client_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (http_url_parse(url, &ctx.url) != 0) {
        return NULL;
    }

    ctx.method              = method;
    ctx.body                = body;
    ctx.body_len            = body_len;
    ctx.content_type        = content_type;
    ctx.redirects_remaining = _http_max_redirects;

    if (xylem_loop_init(&ctx.loop) != 0) {
        return NULL;
    }

    xylem_thrdpool_t* pool = xylem_thrdpool_create(1);
    if (!pool) {
        xylem_loop_deinit(&ctx.loop);
        return NULL;
    }

    for (;;) {
        ctx.done              = false;
        ctx.timed_out         = false;
        ctx.expect_continue   = false;
        ctx.continue_received = false;
        ctx.conn              = NULL;
        ctx.cur_header_name   = NULL;

        ctx.res = calloc(1, sizeof(*ctx.res));
        if (!ctx.res) {
            break;
        }

        llhttp_settings_init(&ctx.settings);
        ctx.settings.on_header_field     = _http_res_header_field_cb;
        ctx.settings.on_header_value     = _http_res_header_value_cb;
        ctx.settings.on_headers_complete = _http_res_headers_complete_cb;
        ctx.settings.on_body             = _http_res_body_cb;
        ctx.settings.on_message_complete = _http_res_message_complete_cb;
        llhttp_init(&ctx.parser, HTTP_RESPONSE, &ctx.settings);
        ctx.parser.data = &ctx;

        xylem_loop_init_timer(&ctx.loop, &ctx.timeout_timer);
        if (_http_timeout_ms > 0) {
            xylem_loop_start_timer(&ctx.timeout_timer,
                                   _http_client_timeout_cb,
                                   _http_timeout_ms, 0);
        }

        xylem_addr_resolve_t* resolve_req =
            xylem_addr_resolve(&ctx.loop, pool, ctx.url.host, ctx.url.port,
                               _http_client_resolve_cb, &ctx);
        if (!resolve_req) {
            _http_client_abort(&ctx);
            xylem_http_cli_res_destroy(ctx.res);
            ctx.res = NULL;
            break;
        }

        xylem_loop_run(&ctx.loop);

        free(ctx.cur_header_name);
        ctx.cur_header_name = NULL;

        if (ctx.timed_out || !ctx.done) {
            xylem_http_cli_res_destroy(ctx.res);
            ctx.res = NULL;
            break;
        }

        if (!_http_client_follow_redirect(&ctx)) {
            break;
        }
    }

    xylem_loop_deinit(&ctx.loop);
    xylem_thrdpool_destroy(pool);
    return ctx.res;
}

xylem_http_cli_res_t* xylem_http_cli_get(const char* url) {
    return _http_client_exec("GET", url, NULL, 0, NULL);
}

xylem_http_cli_res_t* xylem_http_cli_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type) {
    return _http_client_exec("POST", url, body, body_len, content_type);
}

xylem_http_cli_res_t* xylem_http_cli_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type) {
    return _http_client_exec("PUT", url, body, body_len, content_type);
}

xylem_http_cli_res_t* xylem_http_cli_delete(const char* url) {
    return _http_client_exec("DELETE", url, NULL, 0, NULL);
}

xylem_http_cli_res_t* xylem_http_cli_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type) {
    return _http_client_exec("PATCH", url, body, body_len, content_type);
}
