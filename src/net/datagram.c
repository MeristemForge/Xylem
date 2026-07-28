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

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct datagram_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    addr_t          peer_addr;
    bool            connected;
    _Atomic bool    closed;
};

static socklen_t _datagram_addr_len(const addr_t* addr) {
    return (addr->storage.ss_family == AF_INET6)
        ? (socklen_t)sizeof(struct sockaddr_in6)
        : (socklen_t)sizeof(struct sockaddr_in);
}

static iowait_result_t _datagram_wait(
    datagram_t* datagram,
    bool        write) {
    if (atomic_load(&datagram->closed)) {
        return IOWAIT_CLOSED;
    }

    iowait_result_t ret = write
        ? iowait_write(datagram->waiter)
        : iowait_read(datagram->waiter);
    if (atomic_load(&datagram->closed)) {
        ret = IOWAIT_CLOSED;
    }

    return ret;
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

    addr_t* addrs = NULL;
    size_t  count = 0;
    if (addr_lookup(host, port, 0, &addrs, &count) != 0) {
        xylem_loge("<datagram> dial resolve failed host=%s", host);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        socklen_t addr_len = addr_socklen(&addrs[i]);
        if (addr_len == 0) {
            continue;
        }

        bool connected = false;
        platform_sock_t fd = platform_socket_dial(
            (const struct sockaddr*)&addrs[i].storage,
            addr_len,
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
    xylem_loge(
        "<datagram> dial failed host=%s port=%u",
        host,
        (unsigned int)port);
    return NULL;
}

void datagram_close(datagram_t* datagram) {
    if (!atomic_exchange(&datagram->closed, true)) {
        iowait_close(datagram->waiter);
    }
}

void datagram_destroy(datagram_t* datagram) {
    if (!datagram) {
        return;
    }
    /* Detach iowait before closing the fd so a reused fd has no stale poller state. */
    datagram_close(datagram);
    if (datagram->waiter) {
        iowait_destroy(datagram->waiter);
    }
    if (datagram->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(datagram->fd);
    }
    free(datagram);
}

void datagram_set_read_deadline(
    datagram_t* datagram,
    uint64_t    deadline_ms) {
    if (!atomic_load(&datagram->closed)) {
        iowait_set_rd_deadline(datagram->waiter, deadline_ms);
    }
}

void datagram_set_write_deadline(
    datagram_t* datagram,
    uint64_t    deadline_ms) {
    if (!atomic_load(&datagram->closed)) {
        iowait_set_wr_deadline(datagram->waiter, deadline_ms);
    }
}

int datagram_recv(
    datagram_t* datagram,
    void*       buf,
    int         len,
    addr_t*     from) {
    if (!buf || len <= 0) {
        return -1;
    }

    if (atomic_load(&datagram->closed)
        || iowait_read_deadline_expired(datagram->waiter)) {
        return -1;
    }

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

iowait_result_t datagram_wait_read(datagram_t* datagram) {
    return _datagram_wait(datagram, false);
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

    if (atomic_load(&datagram->closed)
        || iowait_write_deadline_expired(datagram->waiter)) {
        return -1;
    }

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

iowait_result_t datagram_wait_write(datagram_t* datagram) {
    return _datagram_wait(datagram, true);
}

int datagram_remote_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    if (atomic_load(&datagram->closed)) {
        return -1;
    }

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
    if (atomic_load(&datagram->closed)) {
        return -1;
    }

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
