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

#include "runtime/runtime.h"

#include "minicoro/minicoro.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    mco_coro* co;
    char*     host;
    addr_t**  addrs;
    size_t*   count;
    int       status;
} _addr_resolve_ctx_t;

static void _addr_resolve_done_cb(
    loop_t* loop,
    loop_post_t* req,
    void* ud) {
    (void)loop;
    (void)req;
    _addr_resolve_ctx_t* ctx = (_addr_resolve_ctx_t*)ud;
    mco_resume(ctx->co);
}

static void _addr_resolve_finish(
    _addr_resolve_ctx_t* ctx,
    addr_t* arr,
    size_t count) {
    *ctx->addrs = arr;
    *ctx->count = count;
    ctx->status = arr ? 0 : -1;
    loop_post(runtime_get_loop(), _addr_resolve_done_cb, ctx);
}

static void _addr_resolve_work(void* arg) {
    _addr_resolve_ctx_t* ctx = (_addr_resolve_ctx_t*)arg;

    struct addrinfo hints;
    struct addrinfo* res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo(ctx->host, NULL, &hints, &res) != 0 || !res) {
        _addr_resolve_finish(ctx, NULL, 0);
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
        _addr_resolve_finish(ctx, NULL, 0);
        return;
    }

    addr_t* arr = (addr_t*)calloc(n, sizeof(addr_t));
    if (!arr) {
        freeaddrinfo(res);
        _addr_resolve_finish(ctx, NULL, 0);
        return;
    }

    size_t i = 0;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        if (rp->ai_family != AF_INET && rp->ai_family != AF_INET6) {
            continue;
        }
        memcpy(&arr[i].storage, rp->ai_addr, rp->ai_addrlen);
        i++;
    }

    freeaddrinfo(res);
    _addr_resolve_finish(ctx, arr, i);
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
    size_t size,
    uint16_t* port) {
    if (!addr || !dst || !port) {
        return -1;
    }

    switch (addr->storage.ss_family) {
    case AF_INET: {
        const struct sockaddr_in* sin =
            (const struct sockaddr_in*)&addr->storage;
        if (!inet_ntop(AF_INET, &sin->sin_addr, dst, (socklen_t)size)) {
            return -1;
        }
        *port = ntohs(sin->sin_port);
        return 0;
    }
    case AF_INET6: {
        const struct sockaddr_in6* sin6 =
            (const struct sockaddr_in6*)&addr->storage;
        if (!inet_ntop(AF_INET6, &sin6->sin6_addr, dst, (socklen_t)size)) {
            return -1;
        }
        *port = ntohs(sin6->sin6_port);
        return 0;
    }
    default:
        return -1;
    }
}

int addr_resolve(
    const char* domain,
    addr_t** addrs,
    size_t* count) {
    if (!domain || !addrs || !count) {
        return -1;
    }

    _addr_resolve_ctx_t ctx;
    ctx.co     = mco_running();
    ctx.host   = (char*)domain;
    ctx.addrs  = addrs;
    ctx.count  = count;
    ctx.status = 0;

    *addrs = NULL;
    *count = 0;

    thrdpool_submit(runtime_get_pool(), _addr_resolve_work, &ctx);
    mco_yield(mco_running());

    return ctx.status;
}
