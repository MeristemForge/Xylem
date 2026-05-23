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

#include "xylem/net/xylem-udp.h"

#include "xylem/xylem-logger.h"

#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xylem_udp_chan_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    addr_t          peer_addr;
    bool            connected;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _udp_chan_ref(xylem_udp_chan_t* udp) {
    atomic_fetch_add_explicit(&udp->refcnt, 1, memory_order_relaxed);
}

static void _udp_chan_unref(xylem_udp_chan_t* udp) {
    if (atomic_fetch_sub_explicit(&udp->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (udp->waiter) {
        iowait_destroy(udp->waiter);
    }
    if (udp->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(udp->fd);
    }
    free(udp);
}

xylem_udp_chan_t* xylem_udp_listen(const char* host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("udp listen: failed for %s:%s", host, port_str);
        return NULL;
    }

    xylem_udp_chan_t* udp = (xylem_udp_chan_t*)calloc(1, sizeof(xylem_udp_chan_t));
    if (!udp) {
        platform_socket_close(fd);
        return NULL;
    }

    udp->fd        = fd;
    udp->connected = false;
    udp->waiter    = iowait_create(fd);
    if (!udp->waiter) {
        platform_socket_close(fd);
        free(udp);
        return NULL;
    }

    _udp_chan_ref(udp);
    return udp;
}

xylem_udp_chan_t* xylem_udp_dial(const char* host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        if (addr_resolve(host, port, &addrs, &count) != 0 || count == 0) {
            xylem_loge("udp dial: DNS resolution failed for %s", host);
            return NULL;
        }
        resolved_addr = addrs[0];
        free(addrs);
        uint16_t rport;
        addr_ntop(&resolved_addr, resolved_ip, sizeof(resolved_ip), &rport);
        dial_host = resolved_ip;
    }

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(
        dial_host, port_str, SOCK_DGRAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("udp dial: failed for %s:%s", host, port_str);
        return NULL;
    }

    xylem_udp_chan_t* udp = (xylem_udp_chan_t*)calloc(1, sizeof(xylem_udp_chan_t));
    if (!udp) {
        platform_socket_close(fd);
        return NULL;
    }

    udp->fd        = fd;
    udp->connected = true;
    udp->peer_addr = resolved_addr;
    udp->waiter    = iowait_create(fd);
    if (!udp->waiter) {
        platform_socket_close(fd);
        free(udp);
        return NULL;
    }

    _udp_chan_ref(udp);
    return udp;
}

int64_t xylem_udp_recv(
    xylem_udp_chan_t* udp,
    void*        buf,
    size_t       len,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    _udp_chan_ref(udp);

    int64_t ret = -1;
    for (;;) {
        if (atomic_load_explicit(&udp->closed, memory_order_acquire)) {
            break;
        }

        ssize_t n;
        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof(sender);

        if (udp->connected) {
            n = platform_socket_recv(udp->fd, buf, (int)len);
        } else {
            n = platform_socket_recvfrom(
                udp->fd, buf, (int)len, &sender, &sender_len);
        }

        if (n >= 0) {
            if (host || port) {
                addr_t addr;
                if (udp->connected) {
                    addr = udp->peer_addr;
                } else {
                    memcpy(&addr.storage, &sender, sizeof(sender));
                }
                addr_ntop(&addr, host, host_len, port);
            }
            ret = (int64_t)n;
            break;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            xylem_loge("udp fd=%d recv error: %s",
                (int)udp->fd, platform_socket_tostring(err));
            break;
        }

        iowait_result_t r = iowait_read(udp->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&udp->closed, memory_order_acquire)) {
            break;
        }
    }

    _udp_chan_unref(udp);
    return ret;
}

int xylem_udp_send(
    xylem_udp_chan_t* udp,
    const void*  data,
    size_t       len,
    const char*  host,
    uint16_t     port) {
    _udp_chan_ref(udp);

    int ret = -1;
    for (;;) {
        if (atomic_load_explicit(&udp->closed, memory_order_acquire)) {
            break;
        }

        ssize_t n;
        if (!host || udp->connected) {
            n = platform_socket_send(udp->fd, data, (int)len);
        } else {
            addr_t dest;
            if (addr_pton(host, port, &dest) != 0) {
                xylem_loge("udp send: host must be numeric IP, got %s",
                           host);
                break;
            }
            socklen_t addrlen =
                (dest.storage.ss_family == AF_INET6)
                    ? (socklen_t)sizeof(struct sockaddr_in6)
                    : (socklen_t)sizeof(struct sockaddr_in);
            n = platform_socket_sendto(
                udp->fd, data, (int)len, &dest.storage, addrlen);
        }

        if (n >= 0) {
            ret = 0;
            break;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            xylem_loge("udp fd=%d send error: %s",
                (int)udp->fd, platform_socket_tostring(err));
            break;
        }

        iowait_result_t r = iowait_write(udp->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&udp->closed, memory_order_acquire)) {
            break;
        }
    }

    _udp_chan_unref(udp);
    return ret;
}

void xylem_udp_set_read_deadline(xylem_udp_chan_t* udp, uint64_t deadline_ms) {
    iowait_set_rd_deadline(udp->waiter, deadline_ms);
}

void xylem_udp_set_write_deadline(xylem_udp_chan_t* udp, uint64_t deadline_ms) {
    iowait_set_wr_deadline(udp->waiter, deadline_ms);
}

void xylem_udp_close(xylem_udp_chan_t* udp) {
    if (atomic_exchange(&udp->closed, true)) {
        return;
    }
    iowait_close(udp->waiter);
    _udp_chan_unref(udp);
}

int xylem_udp_local_addr(
    xylem_udp_chan_t* udp,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(udp->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int xylem_udp_remote_addr(
    xylem_udp_chan_t* udp,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    if (!udp->connected) {
        return -1;
    }
    return addr_ntop(&udp->peer_addr, host, host_len, port);
}
