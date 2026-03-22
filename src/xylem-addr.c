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

#include "xylem/xylem-addr.h"
#include "xylem/xylem-loop.h"
#include "xylem/xylem-thrdpool.h"

#include <stdlib.h>
#include <string.h>

#define _ADDR_RESOLVE_MAX 16

struct xylem_addr_resolve_s {
    xylem_loop_t*            loop;
    xylem_addr_resolve_fn_t  cb;
    void*                    userdata;
    xylem_loop_post_t        post;
    char*                    host;
    uint16_t                 port;
    xylem_addr_t             addrs[_ADDR_RESOLVE_MAX];
    size_t                   count;
    int                      status;
    _Atomic bool             cancelled;
};

/* Runs on the loop thread after the worker finishes. */
static void _addr_resolve_post_cb(xylem_loop_t* loop,
                                  xylem_loop_post_t* req) {
    xylem_addr_resolve_t* r =
        (xylem_addr_resolve_t*)((char*)req -
            offsetof(xylem_addr_resolve_t, post));

    if (!atomic_load(&r->cancelled)) {
        r->cb(r->addrs, r->count, r->status, r->userdata);
    }

    free(r->host);
    free(r);
}

/* Runs on a thread pool worker. */
static void _addr_resolve_work(void* arg) {
    xylem_addr_resolve_t* r = arg;

    if (atomic_load(&r->cancelled)) {
        r->status = -1;
        r->count  = 0;
        r->post.cb = _addr_resolve_post_cb;
        xylem_loop_post(r->loop, &r->post);
        return;
    }

    struct addrinfo hints;
    struct addrinfo* res = NULL;
    char port_str[8];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)r->port);

    if (getaddrinfo(r->host, port_str, &hints, &res) != 0 || !res) {
        r->status = -1;
        r->count  = 0;
        r->post.cb = _addr_resolve_post_cb;
        xylem_loop_post(r->loop, &r->post);
        return;
    }

    size_t count = 0;
    for (struct addrinfo* rp = res;
         rp && count < _ADDR_RESOLVE_MAX;
         rp = rp->ai_next) {
        if (rp->ai_family != AF_INET && rp->ai_family != AF_INET6) {
            continue;
        }
        memset(&r->addrs[count], 0, sizeof(xylem_addr_t));
        memcpy(&r->addrs[count].storage, rp->ai_addr, rp->ai_addrlen);
        count++;
    }

    freeaddrinfo(res);

    r->count  = count;
    r->status = (count > 0) ? 0 : -1;
    r->post.cb = _addr_resolve_post_cb;
    xylem_loop_post(r->loop, &r->post);
}

int xylem_addr_pton(const char* host, uint16_t port, xylem_addr_t* addr) {
    if (!host || !addr) {
        return -1;
    }

    memset(&addr->storage, 0, sizeof(addr->storage));

    /* Try IPv4 first */
    {
        struct sockaddr_in* sin = (struct sockaddr_in*)&addr->storage;
        if (inet_pton(AF_INET, host, &sin->sin_addr) == 1) {
            sin->sin_family = AF_INET;
            sin->sin_port   = htons(port);
            return 0;
        }
    }

    /* Try IPv6 */
    {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&addr->storage;
        if (inet_pton(AF_INET6, host, &sin6->sin6_addr) == 1) {
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port   = htons(port);
            return 0;
        }
    }

    return -1;
}

int xylem_addr_ntop(const xylem_addr_t* addr,
                    char* host, size_t hostlen, uint16_t* port) {
    if (!addr || !host || !port) {
        return -1;
    }

    switch (addr->storage.ss_family) {
    case AF_INET: {
        const struct sockaddr_in* sin =
            (const struct sockaddr_in*)&addr->storage;
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, (socklen_t)hostlen)) {
            return -1;
        }
        *port = ntohs(sin->sin_port);
        return 0;
    }
    case AF_INET6: {
        const struct sockaddr_in6* sin6 =
            (const struct sockaddr_in6*)&addr->storage;
        if (!inet_ntop(AF_INET6, &sin6->sin6_addr, host, (socklen_t)hostlen)) {
            return -1;
        }
        *port = ntohs(sin6->sin6_port);
        return 0;
    }
    default:
        return -1;
    }
}

xylem_addr_resolve_t* xylem_addr_resolve(xylem_loop_t* loop,
                                         xylem_thrdpool_t* pool,
                                         const char* host,
                                         uint16_t port,
                                         xylem_addr_resolve_fn_t cb,
                                         void* userdata) {
    if (!loop || !pool || !host || !cb) {
        return NULL;
    }

    xylem_addr_resolve_t* r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }

    r->host = strdup(host);
    if (!r->host) {
        free(r);
        return NULL;
    }

    r->loop     = loop;
    r->cb       = cb;
    r->userdata = userdata;
    r->port     = port;
    atomic_store(&r->cancelled, false);

    xylem_thrdpool_post(pool, _addr_resolve_work, r);
    return r;
}

void xylem_addr_resolve_cancel(xylem_addr_resolve_t* req) {
    if (req) {
        atomic_store(&req->cancelled, true);
    }
}
