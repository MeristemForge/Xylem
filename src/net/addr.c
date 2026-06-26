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

#include "addr.h"

#include "runtime/precond.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/**
 * Resolve request context.
 *
 * Heap-allocated and reference-counted so the lookup can outlive the
 * waiting coroutine: getaddrinfo is not cancellable, so on timeout the
 * coroutine resumes with an error while the pool thread keeps running
 * the lookup. The completion and the deadline timer race through a
 * single-winner atomic exchange on `waiter`, so the coroutine is woken
 * exactly once; `timed_out` is stamped only by the winning timer.
 *
 * refcnt: 1 (originator/waiter) + 1 (pool job) + 1 (armed timer).
 * The last unref frees host, any unclaimed result, and the timer.
 */
typedef struct _addr_resolve_ctx_s {
    _Atomic(mco_coro*) waiter;
    char*              host;
    uint16_t           port;
    uint64_t           timeout_ms;
    addr_t*            result;
    size_t             result_count;
    int                status;     /* worker outcome: 0 ok, -1 fail */
    _Atomic bool       timed_out;  /* set only by the winning timer */
    scheduler_timer_t*     timer;
    _Atomic int32_t    refcnt;
} _addr_resolve_ctx_t;

static void _addr_ctx_ref(_addr_resolve_ctx_t* ctx) {
    atomic_fetch_add(&ctx->refcnt, 1);
}

static void _addr_ctx_unref(_addr_resolve_ctx_t* ctx) {
    if (atomic_fetch_sub(&ctx->refcnt, 1)
        != 1) {
        return;
    }
    if (ctx->timer) {
        scheduler_timer_destroy(ctx->timer);
    }
    free(ctx->result); /* NULL on success (ownership transferred to caller). */
    free(ctx->host);
    free(ctx);
}

/* Run the blocking lookup and record the outcome into ctx. */
static void _addr_do_lookup(_addr_resolve_ctx_t* ctx) {
    struct addrinfo  hints;
    struct addrinfo* res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo(ctx->host, NULL, &hints, &res) != 0 || !res) {
        return;
    }

    size_t n = 0;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET || rp->ai_family == AF_INET6) {
            n++;
        }
    }
    if (n == 0) {
        freeaddrinfo(res);
        return;
    }

    addr_t* arr = (addr_t*)calloc(n, sizeof(addr_t));
    if (!arr) {
        freeaddrinfo(res);
        return;
    }

    size_t i = 0;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        if (rp->ai_family != AF_INET && rp->ai_family != AF_INET6) {
            continue;
        }
        memcpy(&arr[i].storage, rp->ai_addr, rp->ai_addrlen);
        if (rp->ai_family == AF_INET) {
            ((struct sockaddr_in*)&arr[i].storage)->sin_port =
                htons(ctx->port);
        } else {
            ((struct sockaddr_in6*)&arr[i].storage)->sin6_port =
                htons(ctx->port);
        }
        i++;
    }
    freeaddrinfo(res);

    ctx->result       = arr;
    ctx->result_count = i;
    ctx->status       = 0;
}

static void _addr_resolve_work(void* arg) {
    _addr_resolve_ctx_t* ctx = (_addr_resolve_ctx_t*)arg;

    _addr_do_lookup(ctx);

    /**
     * Single-winner wake: if the timeout already claimed the waiter, co
     * is NULL and the result we just built is discarded (freed by the
     * last unref). Otherwise wake the coroutine.
     */
    mco_coro* co = atomic_exchange(&ctx->waiter, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
    _addr_ctx_unref(ctx);
}

int addr_pton(const char* src, uint16_t port, addr_t* dst) {
    if (!src || !dst) {
        return -1;
    }

    memset(&dst->storage, 0, sizeof(dst->storage));

    {
        struct sockaddr_in* sin = (struct sockaddr_in*)&dst->storage;
        if (inet_pton(AF_INET, src, &sin->sin_addr) == 1) {
            sin->sin_family = AF_INET;
            sin->sin_port = htons(port);
            return 0;
        }
    }

    {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&dst->storage;
        if (inet_pton(AF_INET6, src, &sin6->sin6_addr) == 1) {
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port = htons(port);
            return 0;
        }
    }

    return -1;
}

int addr_ntop(
    const addr_t* addr,
    char* dst,
    size_t dst_len,
    uint16_t* port) {
    if (!addr) {
        return -1;
    }

    int            af;
    const void*    in_addr;
    uint16_t       net_port;

    switch (addr->storage.ss_family) {
    case AF_INET: {
        const struct sockaddr_in* sin =
            (const struct sockaddr_in*)&addr->storage;
        af       = AF_INET;
        in_addr  = &sin->sin_addr;
        net_port = sin->sin_port;
        break;
    }
    case AF_INET6: {
        const struct sockaddr_in6* sin6 =
            (const struct sockaddr_in6*)&addr->storage;
        af       = AF_INET6;
        in_addr  = &sin6->sin6_addr;
        net_port = sin6->sin6_port;
        break;
    }
    default:
        return -1;
    }

    if (dst && !inet_ntop(af, in_addr, dst, (socklen_t)dst_len)) {
        return -1;
    }
    if (port) {
        *port = ntohs(net_port);
    }
    return 0;
}

static void _addr_resolve_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _addr_resolve_ctx_t* ctx = (_addr_resolve_ctx_t*)ud;

    /* Single-winner: only stamp timed_out and wake if we beat the worker. */
    mco_coro* co = atomic_exchange(&ctx->waiter, NULL);
    if (co) {
        atomic_store(&ctx->timed_out, true);
        scheduler_schedule(runtime_get_scheduler(), co);
    }
    _addr_ctx_unref(ctx);
}

static bool _addr_resolve_park_cb(mco_coro* co, void* arg) {
    _addr_resolve_ctx_t* ctx = (_addr_resolve_ctx_t*)arg;

    /* Publish the waiter before submitting so a fast completion sees it. */
    atomic_store(&ctx->waiter, co);

    /* Reference for the pool job. */
    _addr_ctx_ref(ctx);
    if (dynpool_submit(
            runtime_get_dynpool(), _addr_resolve_work, ctx) != 0) {
        /**
         * Submit failed (e.g. OOM). Undo the job ref, reclaim the
         * waiter, and decline the park so the coroutine resumes inline
         * with status == -1. Not declining would orphan it forever.
         */
        _addr_ctx_unref(ctx);
        atomic_store(&ctx->waiter, NULL);
        ctx->status = -1;
        return false;
    }

    /* Arm the deadline timer; one reference for the armed timer. */
    if (ctx->timer) {
        _addr_ctx_ref(ctx);
        scheduler_timer_start(
                ctx->timer,
                _addr_resolve_timeout_cb,
                ctx,
                ctx->timeout_ms,
                0);
    }
    return true;
}

int addr_resolve(
    const char* domain,
    uint16_t port,
    uint64_t timeout_ms,
    addr_t** addrs,
    size_t* count) {
    RUNTIME_REQUIRE_COROUTINE("addr", "addr_resolve");

    if (!domain || !addrs || !count) {
        return -1;
    }

    *addrs = NULL;
    *count = 0;

    _addr_resolve_ctx_t* ctx =
        (_addr_resolve_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }

    size_t hlen = strlen(domain) + 1;
    ctx->host = (char*)malloc(hlen);
    if (!ctx->host) {
        free(ctx);
        return -1;
    }
    memcpy(ctx->host, domain, hlen);

    ctx->port       = port;
    ctx->timeout_ms = timeout_ms;
    ctx->status     = -1;
    atomic_init(&ctx->waiter, NULL);
    atomic_init(&ctx->timed_out, false);
    atomic_init(&ctx->refcnt, 1); /* originator reference */

    /* Best-effort: if the timer cannot be created, fall back to no timeout. */
    if (timeout_ms > 0) {
        ctx->timer = scheduler_timer_create(runtime_get_scheduler());
    }

    scheduler_park(runtime_get_scheduler(), _addr_resolve_park_cb, ctx);

    int rc;
    if (atomic_load(&ctx->timed_out)) {
        /* Timed out: the worker may still run, so do not touch result. */
        rc = -1;
    } else {
        rc = ctx->status;
        if (rc == 0) {
            *addrs = ctx->result;
            *count = ctx->result_count;
            ctx->result = NULL; /* ownership transferred to the caller */
        }

        /**
         * We won the race (or never armed): cancel a still-pending
         * timer and drop its reference if we caught it before it fired.
         */
        if (ctx->timer && scheduler_timer_stop(ctx->timer)) {
            _addr_ctx_unref(ctx);
        }
    }

    _addr_ctx_unref(ctx); /* drop originator reference */
    return rc;
}
