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

_Pragma("once")

#include "xylem.h"
#include "assert.h"

#include "net/http/http.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_OK \
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"

/* Test-only scripted HTTP peer over the public TCP/TLS APIs. */

typedef struct {
    const void* response;
    size_t      response_len; /* 0 = strlen(response). */
    bool        keep_alive;
    bool        reject_body;
    bool        ignore_expect;
    uint64_t    delay_ms;
    size_t      split_at;
    char        request[8192];
} _http_fixture_step_t;
typedef struct {
    void* listener;
    http_transport_t (*accept)(void*);
    void (*close)(void*);
    void (*destroy)(void*);
    _http_fixture_step_t* steps;
    size_t                count;
    _Atomic size_t        next;
    _Atomic size_t        accepted;
    _Atomic bool          stopping;
    xylem_waitgroup_t*    wg;
    uint16_t              port;
} _http_fixture_t;
typedef struct {
    _http_fixture_t* f;
    http_transport_t t;
} _http_fixture_conn_t;

static int _fixture_read(void* p, void* b, int n) {
    return xylem_tcp_read(p, b, n);
}
static int _fixture_write(void* p, const void* b, int n) {
    return xylem_tcp_write(p, b, n);
}
static void _fixture_destroy_conn(void* p) {
    xylem_tcp_destroy(p);
}
static void _fixture_rd(void* p, uint64_t ms) {
    xylem_tcp_set_read_deadline(p, ms);
}
static http_transport_t _fixture_accept(void* p) {
    xylem_tcp_conn_t* c = xylem_tcp_accept(p);
    return (http_transport_t){
        .conn            = c,
        .read            = _fixture_read,
        .write           = _fixture_write,
        .close           = _fixture_destroy_conn,
        .set_rd_deadline = _fixture_rd};
}
static void _fixture_close(void* p) {
    xylem_tcp_close_listener(p);
}
static void _fixture_destroy(void* p) {
    xylem_tcp_destroy_listener(p);
}
#ifdef TEST_WITH_TLS
static int _fixture_tls_read(void* p, void* b, int n) {
    return xylem_tls_read(p, b, n);
}
static int _fixture_tls_write(void* p, const void* b, int n) {
    return xylem_tls_write(p, b, n);
}
static void _fixture_tls_destroy_conn(void* p) {
    xylem_tls_destroy(p);
}
static void _fixture_tls_rd(void* p, uint64_t ms) {
    xylem_tls_set_read_deadline(p, ms);
}
static http_transport_t _fixture_tls_accept(void* p) {
    xylem_tls_conn_t* c = xylem_tls_accept(p);
    return (http_transport_t){
        .conn            = c,
        .read            = _fixture_tls_read,
        .write           = _fixture_tls_write,
        .close           = _fixture_tls_destroy_conn,
        .set_rd_deadline = _fixture_tls_rd};
}
static void _fixture_tls_close(void* p) {
    xylem_tls_close_listener(p);
}
static void _fixture_tls_destroy(void* p) {
    xylem_tls_destroy_listener(p);
}
#endif

static void _fixture_delay_expired_cb(xylem_timer_t* timer, void* userdata) {
    (void)timer;
    xylem_waitgroup_done((xylem_waitgroup_t*)userdata);
}
static void _fixture_wait_delay(uint64_t delay_ms) {
    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg);
    xylem_waitgroup_add(wg, 1);
    xylem_timer_t* timer =
        xylem_timer_after(delay_ms, _fixture_delay_expired_cb, wg);
    ASSERT(timer);
    xylem_waitgroup_wait(wg);
    xylem_timer_destroy(timer);
    xylem_waitgroup_destroy(wg);
}

static void _srv_fixture_conn_cb(void* arg) {
    _http_fixture_conn_t* ctx = arg;
    _http_fixture_t*      f   = ctx->f;
    http_transport_t      t   = ctx->t;
    char                  request[32768];
    size_t                have = 0;
    for (;;) {
        size_t head = 0, wanted = 0;
        bool   sent_continue = false;
        for (;;) {
            request[have] = 0;
            char* end     = strstr(request, "\r\n\r\n");
            if (end && !head) {
                head     = (size_t)(end - request) + 4;
                wanted   = head;
                char* cl = strstr(request, "Content-Length: ");
                if (cl && cl < end) {
                    wanted += (size_t)strtoul(cl + 16, NULL, 10);
                }
                ASSERT(wanted < sizeof(request));
            }
            size_t index = atomic_load(&f->next);
            if (head && !sent_continue &&
                strstr(request, "Expect: 100-continue")) {
                ASSERT(index < f->count);
                if (f->steps[index].reject_body) {
                    wanted = head;
                } else if (!f->steps[index].ignore_expect) {
                    ASSERT(
                        t.write(t.conn, "HTTP/1.1 100 Continue\r\n\r\n", 25) ==
                        0);
                }
                sent_continue = true;
            }
            if (head && have >= wanted) {
                break;
            }
            if (atomic_load(&f->stopping)) {
                goto done;
            }
            t.set_rd_deadline(
                t.conn,
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 2000);
            int n = t.read(
                t.conn,
                request + have,
                (int)(sizeof(request) - 1 - have));
            if (n <= 0) {
                goto done;
            }
            have += (size_t)n;
        }
        size_t index = atomic_fetch_add(&f->next, 1);
        ASSERT(index < f->count);
        _http_fixture_step_t* step = &f->steps[index];
        ASSERT(wanted < sizeof(step->request));
        memcpy(step->request, request, wanted);
        step->request[wanted] = 0;
        have -= wanted;
        memmove(request, request + wanted, have);
        if (step->delay_ms) {
            _fixture_wait_delay(step->delay_ms);
        }
        size_t len =
            step->response_len ? step->response_len : strlen(step->response);
        if (step->split_at && step->split_at < len) {
            if (t.write(t.conn, step->response, (int)step->split_at)) {
                goto done;
            }
            _fixture_wait_delay(5);
            if (t.write(
                    t.conn,
                    (const char*)step->response + step->split_at,
                    (int)(len - step->split_at))) {
                goto done;
            }
        } else if (t.write(t.conn, step->response, (int)len)) {
            goto done;
        }
        if (!step->keep_alive) {
            break;
        }
    }
done:
    t.close(t.conn);
    free(ctx);
    xylem_waitgroup_done(f->wg);
}
static void _srv_fixture_accept_cb(void* arg) {
    _http_fixture_t* f = arg;
    while (!atomic_load(&f->stopping)) {
        http_transport_t t = f->accept(f->listener);
        if (!t.conn) {
            break;
        }
        atomic_fetch_add(&f->accepted, 1);
        _http_fixture_conn_t* c =
            (_http_fixture_conn_t*)malloc(sizeof(_http_fixture_conn_t));
        ASSERT(c);
        c->f = f;
        c->t = t;
        xylem_waitgroup_add(f->wg, 1);
        xylem_spawn(_srv_fixture_conn_cb, c);
    }
    xylem_waitgroup_done(f->wg);
}
static void _fixture_init(
    _http_fixture_t*      f,
    _http_fixture_step_t* steps,
    size_t                count,
    xylem_tls_ctx_t*      tls) {
    (void)tls;
    memset(f, 0, sizeof(*f));
    f->steps = steps;
    f->count = count;
    atomic_init(&f->next, 0);
    atomic_init(&f->accepted, 0);
    atomic_init(&f->stopping, false);
#ifdef TEST_WITH_TLS
    if (tls) {
        f->listener = xylem_tls_listen("127.0.0.1", 0, tls, NULL);
        f->accept   = _fixture_tls_accept;
        f->close    = _fixture_tls_close;
        f->destroy  = _fixture_tls_destroy;
        ASSERT(f->listener);
        ASSERT(xylem_tls_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    }
#endif
    if (!f->listener) {
        f->listener = xylem_tcp_listen("127.0.0.1", 0, NULL);
        f->accept   = _fixture_accept;
        f->close    = _fixture_close;
        f->destroy  = _fixture_destroy;
        ASSERT(f->listener);
        ASSERT(xylem_tcp_listener_addr(f->listener, NULL, 0, &f->port) == 0);
    }
    f->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(f->wg, 1);
    xylem_spawn(_srv_fixture_accept_cb, f);
}
static void _fixture_deinit(_http_fixture_t* f) {
    atomic_store(&f->stopping, true);
    f->close(f->listener);
    xylem_waitgroup_wait(f->wg);
    f->destroy(f->listener);
    xylem_waitgroup_destroy(f->wg);
}
static void
_fixture_url(_http_fixture_t* f, bool secure, const char* path, char url[256]) {
    snprintf(
        url,
        256,
        "%s://127.0.0.1:%u%s",
        secure ? "https" : "http",
        f->port,
        path);
}
static void _check_hello(xylem_http_res_t* r) {
    ASSERT(r);
    ASSERT(xylem_http_res_status(r) == 200);
    ASSERT(xylem_http_res_body_len(r) == 5);
    ASSERT(!memcmp(xylem_http_res_body(r), "hello", 5));
    xylem_http_res_destroy(r);
}
