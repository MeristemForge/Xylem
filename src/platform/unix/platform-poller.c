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
#include <errno.h>
#include <string.h>
#include <unistd.h>

void platform_poller_destroy(platform_poller_sq_t* sq) {
    close(*sq);
}

#if defined(__linux__)
int platform_poller_init(platform_poller_sq_t* sq) {
    *sq = epoll_create1(0);
    return (*sq == -1) ? -1 : 0;
}

int platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    struct epoll_event ee = {0};
    ee.events = EPOLLET | EPOLLONESHOT;

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
    struct epoll_event ee = {0};
    ee.events = EPOLLET | EPOLLONESHOT;

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
    platform_poller_sq_t* sq, platform_poller_cqe_t* cqe,
    int max_events, int timeout) {
    struct epoll_event events[max_events];
    memset(cqe, 0, sizeof(platform_poller_cqe_t) * max_events);

    int n = 0;
    do {
        n = epoll_wait(*sq, events, max_events, timeout);
    } while (n == -1 && errno == EINTR);
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
#endif

#if defined(__APPLE__)
int platform_poller_init(platform_poller_sq_t* sq) {
    *sq = kqueue();
    return (*sq == -1) ? -1 : 0;
}

int platform_poller_add(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    struct kevent ke;
    int           ret = 0;

    if (sqe->op & PLATFORM_POLLER_RD_OP) {
        EV_SET(&ke, sqe->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, sqe->ud);
        if (kevent(*sq, &ke, 1, NULL, 0, NULL) == -1) {
            ret = -1;
        }
    }
    if (sqe->op & PLATFORM_POLLER_WR_OP) {
        EV_SET(&ke, sqe->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, sqe->ud);
        if (kevent(*sq, &ke, 1, NULL, 0, NULL) == -1) {
            ret = -1;
        }
    }
    return ret;
}

int platform_poller_mod(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    struct kevent ke;
    int           ret = 0;

    /* delete filters no longer wanted */
    if (!(sqe->op & PLATFORM_POLLER_RD_OP)) {
        EV_SET(&ke, sqe->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(*sq, &ke, 1, NULL, 0, NULL);
    }
    if (!(sqe->op & PLATFORM_POLLER_WR_OP)) {
        EV_SET(&ke, sqe->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(*sq, &ke, 1, NULL, 0, NULL);
    }
    /* add/re-arm wanted filters */
    if (sqe->op & PLATFORM_POLLER_RD_OP) {
        EV_SET(&ke, sqe->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, sqe->ud);
        if (kevent(*sq, &ke, 1, NULL, 0, NULL) == -1) {
            ret = -1;
        }
    }
    if (sqe->op & PLATFORM_POLLER_WR_OP) {
        EV_SET(&ke, sqe->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, sqe->ud);
        if (kevent(*sq, &ke, 1, NULL, 0, NULL) == -1) {
            ret = -1;
        }
    }
    return ret;
}

int platform_poller_del(platform_poller_sq_t* sq, platform_poller_sqe_t* sqe) {
    struct kevent ke;
    int           ret = 0;

    EV_SET(&ke, sqe->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(*sq, &ke, 1, NULL, 0, NULL);

    EV_SET(&ke, sqe->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(*sq, &ke, 1, NULL, 0, NULL);

    return ret;
}

int platform_poller_wait(
    platform_poller_sq_t* sq, platform_poller_cqe_t* cqe,
    int max_events, int timeout) {
    struct kevent events[max_events];

    memset(cqe, 0, sizeof(platform_poller_cqe_t) * max_events);
    struct timespec ts = {0, 0};
    ts.tv_sec  = (timeout / 1000UL);
    ts.tv_nsec = ((timeout % 1000UL) * 1000000UL);

    int n = kevent(*sq, NULL, 0, events, max_events, &ts);
    if (n < 0) {
        return -1;
    }

    /*
     * kqueue returns separate events for READ and WRITE on the same fd.
     * Merge them into a single cqe entry keyed by ident (fd) to match
     * epoll behavior.
     */
    uintptr_t idents[max_events];
    int       out = 0;
    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < out; j++) {
            if (idents[j] == (uintptr_t)events[i].ident) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            if (events[i].filter == EVFILT_READ) {
                cqe[found].op |= PLATFORM_POLLER_RD_OP;
            }
            if (events[i].filter == EVFILT_WRITE) {
                cqe[found].op |= PLATFORM_POLLER_WR_OP;
            }
        } else {
            idents[out]  = (uintptr_t)events[i].ident;
            cqe[out].ud  = events[i].udata;
            cqe[out].op  = PLATFORM_POLLER_NO_OP;
            if (events[i].filter == EVFILT_READ) {
                cqe[out].op |= PLATFORM_POLLER_RD_OP;
            }
            if (events[i].filter == EVFILT_WRITE) {
                cqe[out].op |= PLATFORM_POLLER_WR_OP;
            }
            out++;
        }
    }
    return out;
}
#endif
