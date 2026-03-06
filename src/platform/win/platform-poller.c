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

#include "platform/platform-poller.h"
#include "wepoll/wepoll.h"
#include <string.h>

int platform_poller_init(platform_poller_sq_t* sq) {
    *sq = epoll_create1(0);
    return (*sq == NULL) ? -1 : 0;
}

void platform_poller_destroy(platform_poller_sq_t* sq) {
    epoll_close(*sq);
}

int platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    if (sqe->op & PLATFORM_POLLER_RD_OP) {
        ee.events |= EPOLLIN;
    }
    if (sqe->op & PLATFORM_POLLER_WR_OP) {
        ee.events |= EPOLLOUT;
    }
    ee.data.ptr = sqe->ud;
    return epoll_ctl(*sq, EPOLL_CTL_ADD, sqe->fd, &ee);
}

int platform_poller_mod(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));
    if (sqe->op & PLATFORM_POLLER_RD_OP) {
        ee.events |= EPOLLIN;
    }
    if (sqe->op & PLATFORM_POLLER_WR_OP) {
        ee.events |= EPOLLOUT;
    }
    ee.data.ptr = sqe->ud;
    return epoll_ctl(*sq, EPOLL_CTL_MOD, sqe->fd, &ee);
}

int platform_poller_del(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    return epoll_ctl(*sq, EPOLL_CTL_DEL, sqe->fd, NULL);
}

int platform_poller_wait(
    platform_poller_sq_t* sq, platform_poller_cqe_t* cqe, int timeout) {
    struct epoll_event events[PLATFORM_POLLER_CQE_NUM];
    memset(cqe, 0, sizeof(platform_poller_cqe_t) * PLATFORM_POLLER_CQE_NUM);

    int n = epoll_wait(*sq, events, PLATFORM_POLLER_CQE_NUM, timeout);
    if (n < 0) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        cqe[i].ud = events[i].data.ptr;
        if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            cqe[i].op |= PLATFORM_POLLER_RD_OP;
        }
        if (events[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            cqe[i].op |= PLATFORM_POLLER_WR_OP;
        }
    }
    return n;
}
