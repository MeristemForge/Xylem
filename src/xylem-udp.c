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

#include "xylem/xylem-udp.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-list.h"

#include <stdlib.h>
#include <string.h>

struct xylem_udp_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t       io;
    platform_sock_t       fd;
    xylem_udp_handler_t*  handler;
    void*                 userdata;
    char                  recv_buf[65536];
    bool                  closing;
    xylem_loop_post_t     free_post;
};

/**
 * Handle readable event: recvfrom into recv_buf, wrap sender in
 * xylem_addr_t, call handler->on_read.
 */
static void _udp_io_cb(xylem_loop_t* loop,
                        xylem_loop_io_t* io,
                        platform_poller_op_t revents) {
    (void)loop;
    (void)revents;
    xylem_udp_t* udp = xylem_list_entry(io, xylem_udp_t, io);

    if (udp->closing) {
        return;
    }

    /**
     * Loop recvfrom until EAGAIN to drain the kernel buffer in one
     * IO callback, avoiding repeated poller wakeups under LT.
     */
    for (;;) {
        struct sockaddr_storage sender;
        socklen_t              sender_len = sizeof(sender);

        ssize_t n = platform_socket_recvfrom(udp->fd, udp->recv_buf,
                                             (int)sizeof(udp->recv_buf),
                                             &sender, &sender_len);
        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                return;
            }
            xylem_logw("udp fd=%d recvfrom error=%d", (int)udp->fd, err);
            return;
        }

        if (n > 0 && udp->handler && udp->handler->on_read) {
            xylem_addr_t addr;
            memcpy(&addr.storage, &sender, sizeof(sender));
            udp->handler->on_read(udp, udp->recv_buf, (size_t)n, &addr);
        }

        if (udp->closing) {
            return;
        }
    }
}

/* Post callback: free a UDP handle after the current iteration. */
static void _udp_free_cb(xylem_loop_t* loop, xylem_loop_post_t* req) {
    (void)loop;
    xylem_udp_t* udp = xylem_list_entry(req, xylem_udp_t, free_post);
    free(udp);
}

xylem_udp_t* xylem_udp_bind(xylem_loop_t* loop,
                             xylem_addr_t* addr,
                             xylem_udp_handler_t* handler) {
    xylem_udp_t* udp = (xylem_udp_t*)calloc(1, sizeof(xylem_udp_t));
    if (!udp) {
        return NULL;
    }

    /* Determine address family and address length */
    int       af      = (int)addr->storage.ss_family;
    socklen_t addrlen = (af == AF_INET6)
                            ? (socklen_t)sizeof(struct sockaddr_in6)
                            : (socklen_t)sizeof(struct sockaddr_in);

    /* Create UDP socket */
    platform_sock_t fd = (platform_sock_t)socket(af, SOCK_DGRAM,
                                                  IPPROTO_UDP);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(udp);
        xylem_loge("udp bind: socket creation failed");
        return NULL;
    }

    platform_socket_enable_nonblocking(fd, true);
    platform_socket_enable_reuseaddr(fd, true);

    if (bind(fd, (struct sockaddr*)&addr->storage, addrlen) != 0) {
        xylem_loge("udp bind: bind() failed");
        platform_socket_close(fd);
        free(udp);
        return NULL;
    }

    udp->loop    = loop;
    udp->fd      = fd;
    udp->handler = handler;
    udp->closing = false;

    xylem_loop_init_io(loop, &udp->io, fd);
    xylem_loop_start_io(&udp->io, PLATFORM_POLLER_RD_OP, _udp_io_cb);

    xylem_logi("udp fd=%d bound", (int)fd);
    return udp;
}

int xylem_udp_send(xylem_udp_t* udp, xylem_addr_t* dest,
                   const void* data, size_t len) {
    socklen_t addrlen = (dest->storage.ss_family == AF_INET6)
                            ? (socklen_t)sizeof(struct sockaddr_in6)
                            : (socklen_t)sizeof(struct sockaddr_in);

    ssize_t n = platform_socket_sendto(udp->fd, data, (int)len,
                                       &dest->storage, addrlen);
    return (n < 0) ? -1 : (int)n;
}

int xylem_udp_join_mcast(xylem_udp_t* udp, const char* group) {
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
        return -1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(udp->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mreq, sizeof(mreq)) == 0 ? 0 : -1;
}

int xylem_udp_leave_mcast(xylem_udp_t* udp, const char* group) {
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
        return -1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(udp->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                      (const char*)&mreq, sizeof(mreq)) == 0 ? 0 : -1;
}

void xylem_udp_close(xylem_udp_t* udp) {
    if (udp->closing) {
        return;
    }
    xylem_logd("udp fd=%d closing", (int)udp->fd);
    udp->closing = true;

    xylem_loop_stop_io(&udp->io);
    udp->loop->active_count--;
    platform_socket_close(udp->fd);

    if (udp->handler && udp->handler->on_close) {
        udp->handler->on_close(udp, 0);
    }

    /* Defer free to next loop iteration so close_node stays valid */
    udp->free_post.cb = _udp_free_cb;
    xylem_loop_post(udp->loop, &udp->free_post);
}

void* xylem_udp_get_userdata(xylem_udp_t* udp) {
    return udp->userdata;
}

void xylem_udp_set_userdata(xylem_udp_t* udp, void* ud) {
    udp->userdata = ud;
}
