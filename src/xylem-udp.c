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

#include "platform/platform-socket.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xylem_udp_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t*      io;
    platform_sock_t       fd;
    xylem_udp_handler_t*  handler;
    void*                 userdata;
    xylem_addr_t          peer;
    char                  recv_buf[65536];
    bool                  connected;
    _Atomic bool          closing;
    _Atomic int32_t       refcount;
    int                   close_err;
    const char*           close_errmsg;
};

typedef struct _udp_deferred_send_s {
    xylem_udp_t*  udp;
    xylem_addr_t  dest;
    bool          has_dest;
    size_t        len;
    char          data[];
} _udp_deferred_send_t;

/**
 * Handle readable event: recvfrom into recv_buf, wrap sender in
 * xylem_addr_t, call handler->on_read.
 */
static void _udp_io_cb(xylem_loop_t* loop,
                        xylem_loop_io_t* io,
                        xylem_poller_op_t revents,
                        void* ud) {
    (void)loop;
    (void)io;
    (void)revents;
    xylem_udp_t* udp = (xylem_udp_t*)ud;

    if (atomic_load(&udp->closing)) {
        return;
    }

    for (;;) {
        ssize_t n;
        xylem_addr_t addr;

        if (udp->connected) {
            n = platform_socket_recv(udp->fd, udp->recv_buf,
                                     (int)sizeof(udp->recv_buf));
            addr = udp->peer;
        } else {
            struct sockaddr_storage sender;
            socklen_t              sender_len = sizeof(sender);
            n = platform_socket_recvfrom(udp->fd, udp->recv_buf,
                                         (int)sizeof(udp->recv_buf),
                                         &sender, &sender_len);
            memcpy(&addr.storage, &sender, sizeof(sender));
        }

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                return;
            }
            xylem_logw("udp fd=%d recv error=%d (%s)",
                       (int)udp->fd, err,
                       platform_socket_tostring(err));
            udp->close_err    = err;
            udp->close_errmsg = platform_socket_tostring(err);
            xylem_udp_close(udp);
            return;
        }

        if (n >= 0 && udp->handler && udp->handler->on_read) {
            udp->handler->on_read(udp, udp->recv_buf, (size_t)n, &addr);
        }

        if (atomic_load(&udp->closing)) {
            return;
        }
    }
}

static void _udp_decref(xylem_udp_t* udp) {
    if (atomic_fetch_sub(&udp->refcount, 1) == 1) {
        free(udp);
    }
}

/* Post callback: free a UDP handle after the current iteration. */
static void _udp_free_cb(xylem_loop_t* loop, xylem_loop_post_t* req,
                         void* ud) {
    (void)loop;
    (void)req;
    _udp_decref((xylem_udp_t*)ud);
}

xylem_udp_t* xylem_udp_listen(xylem_loop_t* loop,
                             xylem_addr_t* addr,
                             xylem_udp_handler_t* handler) {
    xylem_udp_t* udp = (xylem_udp_t*)calloc(1, sizeof(xylem_udp_t));
    if (!udp) {
        return NULL;
    }

    char host[INET6_ADDRSTRLEN];
    char port_str[8];
    uint16_t port = 0;
    xylem_addr_ntop(addr, host, sizeof(host), &port);
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd = platform_socket_listen(host, port_str,
                                                SOCK_DGRAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(udp);
        xylem_loge("udp bind: socket creation failed for %s:%s", host, port_str);
        return NULL;
    }

    udp->loop      = loop;
    udp->fd        = fd;
    udp->handler   = handler;
    udp->connected = false;
    udp->closing   = false;
    atomic_store(&udp->refcount, 1);

    udp->io = xylem_loop_create_io(loop, fd);
    if (!udp->io) {
        platform_socket_close(fd);
        free(udp);
        xylem_loge("udp fd=%d bind: io creation failed", (int)fd);
        return NULL;
    }
    xylem_loop_start_io(udp->io, XYLEM_POLLER_RD_OP, _udp_io_cb, udp);

    xylem_logi("udp fd=%d bound on %s:%s", (int)fd, host, port_str);
    return udp;
}

xylem_udp_t* xylem_udp_dial(xylem_loop_t* loop,
                             xylem_addr_t* addr,
                             xylem_udp_handler_t* handler) {
    xylem_udp_t* udp = (xylem_udp_t*)calloc(1, sizeof(xylem_udp_t));
    if (!udp) {
        return NULL;
    }

    char host[INET6_ADDRSTRLEN];
    char port_str[8];
    uint16_t port = 0;
    xylem_addr_ntop(addr, host, sizeof(host), &port);
    snprintf(port_str, sizeof(port_str), "%u", port);

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(host, port_str,
                                              SOCK_DGRAM,
                                              &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(udp);
        xylem_loge("udp dial: socket creation failed for %s:%s", host, port_str);
        return NULL;
    }

    udp->loop      = loop;
    udp->fd        = fd;
    udp->handler   = handler;
    udp->peer      = *addr;
    udp->connected = true;
    udp->closing   = false;
    atomic_store(&udp->refcount, 1);

    udp->io = xylem_loop_create_io(loop, fd);
    if (!udp->io) {
        platform_socket_close(fd);
        free(udp);
        xylem_loge("udp fd=%d dial: io creation failed", (int)fd);
        return NULL;
    }
    xylem_loop_start_io(udp->io, XYLEM_POLLER_RD_OP, _udp_io_cb, udp);

    xylem_logi("udp fd=%d connected to %s:%s", (int)fd, host, port_str);
    return udp;
}

static void _udp_deferred_send_cb(xylem_loop_t* loop,
                                   xylem_loop_post_t* req,
                                   void* ud) {
    (void)loop;
    (void)req;
    _udp_deferred_send_t* ds = (_udp_deferred_send_t*)ud;

    if (!atomic_load(&ds->udp->closing)) {
        xylem_udp_send(ds->udp, ds->has_dest ? &ds->dest : NULL,
                       ds->data, ds->len);
    }

    _udp_decref(ds->udp);
    free(ds);
}

int xylem_udp_send(xylem_udp_t* udp, xylem_addr_t* dest,
                   const void* data, size_t len) {
    if (atomic_load(&udp->closing)) {
        xylem_logd("udp fd=%d send rejected (closing)", (int)udp->fd);
        return -1;
    }

    /* Cross-thread: copy data and post to loop thread. */
    if (!xylem_loop_is_loop_thread(udp->loop)) {
        _udp_deferred_send_t* ds = (_udp_deferred_send_t*)malloc(
            sizeof(_udp_deferred_send_t) + len);
        if (!ds) {
            return -1;
        }
        ds->udp      = udp;
        ds->has_dest = (dest != NULL);
        if (dest) {
            ds->dest = *dest;
        }
        ds->len = len;
        memcpy(ds->data, data, len);

        atomic_fetch_add(&udp->refcount, 1);
        if (xylem_loop_post(udp->loop, _udp_deferred_send_cb, ds) != 0) {
            _udp_decref(udp);
            free(ds);
            return -1;
        }
        return 0;
    }

    /* Same thread: send directly. */
    if (!dest || udp->connected) {
        ssize_t n = platform_socket_send(udp->fd, data, (int)len);
        if (n < 0) {
            int err = platform_socket_get_lasterror();
            xylem_logw("udp fd=%d send error=%d (%s)", (int)udp->fd,
                       err, platform_socket_tostring(err));
        }
        return (n < 0) ? -1 : (int)n;
    }

    socklen_t addrlen = (dest->storage.ss_family == AF_INET6)
                            ? (socklen_t)sizeof(struct sockaddr_in6)
                            : (socklen_t)sizeof(struct sockaddr_in);

    ssize_t n = platform_socket_sendto(udp->fd, data, (int)len,
                                       &dest->storage, addrlen);
    if (n < 0) {
        int err = platform_socket_get_lasterror();
        xylem_logw("udp fd=%d sendto error=%d (%s)", (int)udp->fd,
                   err, platform_socket_tostring(err));
    }
    return (n < 0) ? -1 : (int)n;
}

static void _udp_do_close(xylem_udp_t* udp) {
    xylem_logi("udp fd=%d closing", (int)udp->fd);

    xylem_loop_destroy_io(udp->io);
    platform_socket_close(udp->fd);

    if (udp->handler && udp->handler->on_close) {
        udp->handler->on_close(udp, udp->close_err, udp->close_errmsg);
    }

    xylem_loop_post(udp->loop, _udp_free_cb, udp);
}

static void _udp_deferred_close_cb(xylem_loop_t* loop,
                                    xylem_loop_post_t* req,
                                    void* ud) {
    (void)loop;
    (void)req;
    xylem_udp_t* udp = (xylem_udp_t*)ud;
    _udp_do_close(udp);
    _udp_decref(udp);
}

void xylem_udp_close(xylem_udp_t* udp) {
    /* Idempotent: may be called multiple times or from callbacks. */
    if (atomic_load(&udp->closing)) {
        return;
    }
    atomic_store(&udp->closing, true);

    /* Cross-thread: post to loop thread. */
    if (!xylem_loop_is_loop_thread(udp->loop)) {
        atomic_fetch_add(&udp->refcount, 1);
        if (xylem_loop_post(udp->loop, _udp_deferred_close_cb, udp) != 0) {
            _udp_decref(udp);
        }
        return;
    }

    _udp_do_close(udp);
}

xylem_loop_t* xylem_udp_get_loop(xylem_udp_t* udp) {
    return udp->loop;
}

void* xylem_udp_get_userdata(xylem_udp_t* udp) {
    return udp->userdata;
}

void xylem_udp_set_userdata(xylem_udp_t* udp, void* ud) {
    udp->userdata = ud;
}

void xylem_udp_acquire(xylem_udp_t* udp) {
    atomic_fetch_add(&udp->refcount, 1);
}

void xylem_udp_release(xylem_udp_t* udp) {
    _udp_decref(udp);
}
