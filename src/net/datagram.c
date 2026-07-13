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

#include "net/datagram.h"

#include "xylem/xylem-logger.h"

#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct datagram_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    addr_t          peer_addr;
    bool            connected;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _datagram_ref(datagram_t* datagram) {
    atomic_fetch_add(&datagram->refcnt, 1);
}

static void _datagram_unref(datagram_t* datagram) {
    if (atomic_fetch_sub(&datagram->refcnt, 1) != 1) {
        return;
    }

    if (datagram->waiter) {
        /**
         * Disarm any in-flight deadline timer before teardown. iowait
         * close/destroy do not cancel timers, and an armed timer holds
         * an iowait reference until the stale deadline fires.
         */
        iowait_set_rd_deadline(datagram->waiter, 0);
        iowait_set_wr_deadline(datagram->waiter, 0);
        iowait_destroy(datagram->waiter);
    }
    if (datagram->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(datagram->fd);
    }
    free(datagram);
}

static socklen_t _datagram_addr_len(const addr_t* addr) {
    return (addr->storage.ss_family == AF_INET6)
        ? (socklen_t)sizeof(struct sockaddr_in6)
        : (socklen_t)sizeof(struct sockaddr_in);
}

static iowait_result_t _datagram_wait(
    datagram_t* datagram,
    bool        write) {
    _datagram_ref(datagram);

    if (atomic_load(&datagram->closed)) {
        _datagram_unref(datagram);
        return IOWAIT_CLOSED;
    }

    iowait_result_t ret = write
        ? iowait_write(datagram->waiter)
        : iowait_read(datagram->waiter);
    if (atomic_load(&datagram->closed)) {
        ret = IOWAIT_CLOSED;
    }

    _datagram_unref(datagram);
    return ret;
}

static int _datagram_recv_once(
    datagram_t* datagram,
    void*       buf,
    int         len,
    addr_t*     from) {
    struct sockaddr_storage sender;
    socklen_t               sender_len = sizeof(sender);
    ssize_t                 n;
    if (datagram->connected) {
        n = platform_socket_recv(datagram->fd, buf, len);
    } else {
        n = platform_socket_recvfrom(
            datagram->fd,
            buf,
            len,
            &sender,
            &sender_len);
    }

    if (n >= 0) {
        if (from) {
            if (datagram->connected) {
                *from = datagram->peer_addr;
            } else {
                memcpy(&from->storage, &sender, sizeof(sender));
            }
        }
        if (runtime_consume_credit(RUNTIME_IO_CREDIT_COST)) {
            runtime_yield();
        }
        return (int)n;
    }

    int err = platform_socket_get_lasterror();
    if (err == PLATFORM_SO_ERROR_EAGAIN
        || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
        return DATAGRAM_IO_AGAIN;
    }

    xylem_loge(
        "<datagram> recv failed fd=%d err=%s",
        (int)datagram->fd,
        platform_socket_tostring(err));
    return -1;
}

static int _datagram_send_once(
    datagram_t*   datagram,
    const void*   data,
    int           len,
    const addr_t* to) {
    ssize_t n;
    if (datagram->connected) {
        n = platform_socket_send(datagram->fd, data, len);
    } else {
        n = platform_socket_sendto(
            datagram->fd,
            data,
            len,
            &to->storage,
            _datagram_addr_len(to));
    }

    if (n >= 0) {
        if (runtime_consume_credit(RUNTIME_IO_CREDIT_COST)) {
            runtime_yield();
        }
        return (int)n;
    }

    int err = platform_socket_get_lasterror();
    if (err == PLATFORM_SO_ERROR_EAGAIN
        || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
        return DATAGRAM_IO_AGAIN;
    }

    xylem_loge(
        "<datagram> send failed fd=%d err=%s",
        (int)datagram->fd,
        platform_socket_tostring(err));
    return -1;
}

datagram_t* datagram_from_fd(
    platform_sock_t fd,
    bool            connected,
    const addr_t*   peer_addr) {
    if (connected && !peer_addr) {
        return NULL;
    }

    datagram_t* datagram = (datagram_t*)calloc(1, sizeof(datagram_t));
    if (!datagram) {
        return NULL;
    }

    datagram->fd        = fd;
    datagram->connected = connected;
    if (connected) {
        datagram->peer_addr = *peer_addr;
    }

    datagram->waiter = iowait_create(fd);
    if (!datagram->waiter) {
        free(datagram);
        return NULL;
    }

    atomic_init(&datagram->refcnt, 1);
    atomic_init(&datagram->closed, false);
    return datagram;
}

datagram_t* datagram_listen(const char* host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true, false);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge(
            "<datagram> listen failed host=%s port=%s",
            host ? host : "*",
            port_str);
        return NULL;
    }

    datagram_t* datagram = datagram_from_fd(fd, false, NULL);
    if (!datagram) {
        platform_socket_close(fd);
        return NULL;
    }
    return datagram;
}

datagram_t* datagram_dial(const char* host, uint16_t port) {
    if (!host || !*host) {
        return NULL;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    addr_t* addrs = NULL;
    size_t  count = 0;
    if (addr_lookup(host, port, 0, &addrs, &count) != 0) {
        xylem_loge("<datagram> dial resolve failed host=%s", host);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        char resolved_ip[INET6_ADDRSTRLEN];
        if (addr_ntop(&addrs[i], resolved_ip, sizeof(resolved_ip), NULL) != 0) {
            continue;
        }

        bool connected = false;
        platform_sock_t fd = platform_socket_dial(
            resolved_ip,
            port_str,
            SOCK_DGRAM,
            &connected,
            true,
            false);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            continue;
        }

        datagram_t* datagram = datagram_from_fd(fd, true, &addrs[i]);
        if (!datagram) {
            platform_socket_close(fd);
            break;
        }

        free(addrs);
        return datagram;
    }

    free(addrs);
    xylem_loge("<datagram> dial failed host=%s port=%s", host, port_str);
    return NULL;
}

void datagram_interrupt(datagram_t* datagram) {
    if (atomic_exchange(&datagram->closed, true)) {
        return;
    }
    iowait_close(datagram->waiter);
}

void datagram_release(datagram_t* datagram) {
    _datagram_unref(datagram);
}

void datagram_set_read_deadline(
    datagram_t* datagram,
    uint64_t    deadline_ms) {
    iowait_set_rd_deadline(datagram->waiter, deadline_ms);
}

void datagram_set_write_deadline(
    datagram_t* datagram,
    uint64_t    deadline_ms) {
    iowait_set_wr_deadline(datagram->waiter, deadline_ms);
}

int datagram_try_recv(
    datagram_t* datagram,
    void*       buf,
    int         len,
    addr_t*     from) {
    if (!buf || len <= 0) {
        return -1;
    }

    _datagram_ref(datagram);
    if (atomic_load(&datagram->closed)
        || iowait_read_deadline_expired(datagram->waiter)) {
        _datagram_unref(datagram);
        return -1;
    }

    int ret = _datagram_recv_once(datagram, buf, len, from);

    _datagram_unref(datagram);
    return ret;
}

iowait_result_t datagram_wait_read(datagram_t* datagram) {
    return _datagram_wait(datagram, false);
}

int datagram_recv(
    datagram_t* datagram,
    void*       buf,
    int         len,
    addr_t*     from) {
    if (!buf || len <= 0) {
        return -1;
    }

    _datagram_ref(datagram);
    int ret = -1;

    for (;;) {
        if (atomic_load(&datagram->closed)
            || iowait_read_deadline_expired(datagram->waiter)) {
            break;
        }

        ret = _datagram_recv_once(datagram, buf, len, from);
        if (ret != DATAGRAM_IO_AGAIN) {
            break;
        }
        if (iowait_read(datagram->waiter) == IOWAIT_READY) {
            continue;
        }
        ret = -1;
        break;
    }

    _datagram_unref(datagram);
    return ret;
}

int datagram_try_send(
    datagram_t*   datagram,
    const void*   data,
    int           len,
    const addr_t* to) {
    if (len < 0 || (len > 0 && !data)) {
        return -1;
    }
    if ((datagram->connected && to) || (!datagram->connected && !to)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    _datagram_ref(datagram);
    if (atomic_load(&datagram->closed)
        || iowait_write_deadline_expired(datagram->waiter)) {
        _datagram_unref(datagram);
        return -1;
    }

    int ret = _datagram_send_once(datagram, data, len, to);

    _datagram_unref(datagram);
    return ret;
}

iowait_result_t datagram_wait_write(datagram_t* datagram) {
    return _datagram_wait(datagram, true);
}

int datagram_send(
    datagram_t*   datagram,
    const void*   data,
    int           len,
    const addr_t* to) {
    if (len < 0 || (len > 0 && !data)) {
        return -1;
    }
    if ((datagram->connected && to) || (!datagram->connected && !to)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    _datagram_ref(datagram);
    int ret = -1;

    for (;;) {
        if (atomic_load(&datagram->closed)
            || iowait_write_deadline_expired(datagram->waiter)) {
            break;
        }

        int n = _datagram_send_once(datagram, data, len, to);
        if (n >= 0) {
            ret = 0;
            break;
        }
        if (n != DATAGRAM_IO_AGAIN) {
            break;
        }
        if (iowait_write(datagram->waiter) == IOWAIT_READY) {
            continue;
        }
        break;
    }

    _datagram_unref(datagram);
    return ret;
}

int datagram_remote_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    if (!datagram->connected) {
        return -1;
    }
    return addr_ntop(&datagram->peer_addr, host, host_len, port);
}

int datagram_local_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    addr_t    addr = {0};
    socklen_t len  = sizeof(addr.storage);
    if (getsockname(datagram->fd, (struct sockaddr*)&addr.storage, &len)
        != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

platform_sock_t datagram_fd(datagram_t* datagram) {
    return datagram->fd;
}
