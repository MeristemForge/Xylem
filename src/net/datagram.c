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
    bool            peer_addr_valid;
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

static bool _datagram_is_again(int err) {
    return err == PLATFORM_SO_ERROR_EAGAIN
           || err == PLATFORM_SO_ERROR_EWOULDBLOCK;
}

static socklen_t _datagram_addr_len(const addr_t* addr) {
    return (addr->storage.ss_family == AF_INET6)
        ? (socklen_t)sizeof(struct sockaddr_in6)
        : (socklen_t)sizeof(struct sockaddr_in);
}

static void _datagram_consume_io_budget(size_t bytes) {
    bool should_yield = runtime_consume_credit(1);
    if (runtime_consume_io_credit(bytes)) {
        should_yield = true;
    }
    if (should_yield) {
        runtime_yield_credit();
    }
}

static iowait_result_t _datagram_wait_result(
    datagram_t* datagram,
    bool        write) {
    _datagram_ref(datagram);
    iowait_result_t ret = IOWAIT_CLOSED;

    if (!atomic_load(&datagram->closed)) {
        iowait_result_t r = write ? iowait_write(datagram->waiter)
                                  : iowait_read(datagram->waiter);
        if (!atomic_load(&datagram->closed)) {
            ret = r;
        }
    }

    _datagram_unref(datagram);
    return ret;
}

static int _datagram_load_peer_addr(datagram_t* datagram) {
    if (datagram->peer_addr_valid) {
        return 0;
    }

    socklen_t peer_len = sizeof(datagram->peer_addr.storage);
    if (getpeername(
            datagram->fd,
            (struct sockaddr*)&datagram->peer_addr.storage,
            &peer_len)
        != 0) {
        return -1;
    }
    datagram->peer_addr_valid = true;
    return 0;
}

datagram_t* datagram_from_fd(
    platform_sock_t fd,
    bool            connected,
    const addr_t*   peer_addr) {
    datagram_t* datagram = (datagram_t*)calloc(1, sizeof(datagram_t));
    if (!datagram) {
        return NULL;
    }

    datagram->fd        = fd;
    datagram->connected = connected;
    if (connected && peer_addr) {
        datagram->peer_addr       = *peer_addr;
        datagram->peer_addr_valid = true;
    }

    datagram->waiter = iowait_create(fd);
    if (!datagram->waiter) {
        free(datagram);
        return NULL;
    }

    _datagram_ref(datagram);
    return datagram;
}

datagram_t* datagram_listen(const char* host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<datagram> listen failed host=%s port=%s", host, port_str);
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
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        if (addr_resolve(host, port, 0, &addrs, &count) != 0 || count == 0) {
            xylem_loge("<datagram> dial dns failed host=%s", host);
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
        xylem_loge("<datagram> dial failed host=%s port=%s", host, port_str);
        return NULL;
    }

    datagram_t* datagram = datagram_from_fd(fd, true, &resolved_addr);
    if (!datagram) {
        platform_socket_close(fd);
        return NULL;
    }
    return datagram;
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
    addr_t*     from,
    bool*       again) {
    if (!buf || len <= 0) {
        *again = false;
        return -1;
    }

    _datagram_ref(datagram);
    int ret = -1;
    *again  = false;

    if (!atomic_load(&datagram->closed)) {
        if (iowait_read_deadline_expired(datagram->waiter)) {
            _datagram_unref(datagram);
            return ret;
        }

        ssize_t n;
        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof(sender);

        if (datagram->connected) {
            n = platform_socket_recv(datagram->fd, buf, len);
        } else {
            n = platform_socket_recvfrom(
                datagram->fd, buf, len, &sender, &sender_len);
        }

        if (n >= 0) {
            ret = (int)n;
            if (from) {
                if (datagram->connected) {
                    if (_datagram_load_peer_addr(datagram) == 0) {
                        *from = datagram->peer_addr;
                    }
                } else {
                    memcpy(&from->storage, &sender, sizeof(sender));
                }
            }
            if (n > 0) {
                _datagram_consume_io_budget((size_t)n);
            }
        } else {
            int err = platform_socket_get_lasterror();
            if (_datagram_is_again(err)) {
                *again = true;
            } else {
                xylem_loge(
                    "<datagram> recv failed fd=%d err=%s",
                    (int)datagram->fd,
                    platform_socket_tostring(err));
            }
        }
    }

    _datagram_unref(datagram);
    return ret;
}

int datagram_wait_read(datagram_t* datagram) {
    return datagram_wait_read_result(datagram) == IOWAIT_READY ? 0 : -1;
}

iowait_result_t datagram_wait_read_result(datagram_t* datagram) {
    return _datagram_wait_result(datagram, false);
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

    if (!atomic_load(&datagram->closed)) {
        for (;;) {
            bool again = false;
            int  n     = datagram_try_recv(datagram, buf, len, from, &again);
            if (n >= 0) {
                ret = n;
                break;
            }
            if (!again || datagram_wait_read(datagram) != 0) {
                break;
            }
        }
    }

    _datagram_unref(datagram);
    return ret;
}

int datagram_try_send(
    datagram_t*  datagram,
    const void*  data,
    int          len,
    const addr_t* to,
    bool*        again) {
    if (len < 0) {
        *again = false;
        return -1;
    }
    if (len == 0) {
        *again = false;
        return 0;
    }
    if (!data) {
        *again = false;
        return -1;
    }

    _datagram_ref(datagram);
    int ret = -1;
    *again  = false;

    if (!atomic_load(&datagram->closed)) {
        if (iowait_write_deadline_expired(datagram->waiter)) {
            _datagram_unref(datagram);
            return ret;
        }

        ssize_t n;
        if (!to || datagram->connected) {
            n = platform_socket_send(datagram->fd, data, len);
        } else {
            addr_t dest = *to;
            n = platform_socket_sendto(
                datagram->fd,
                data,
                len,
                &dest.storage,
                _datagram_addr_len(&dest));
        }

        if (n >= 0) {
            ret = (int)n;
            if (n > 0) {
                _datagram_consume_io_budget((size_t)n);
            }
        } else {
            int err = platform_socket_get_lasterror();
            if (_datagram_is_again(err)) {
                *again = true;
            } else {
                xylem_loge(
                    "<datagram> send failed fd=%d err=%s",
                    (int)datagram->fd,
                    platform_socket_tostring(err));
            }
        }
    }

    _datagram_unref(datagram);
    return ret;
}

int datagram_wait_write(datagram_t* datagram) {
    return datagram_wait_write_result(datagram) == IOWAIT_READY ? 0 : -1;
}

iowait_result_t datagram_wait_write_result(datagram_t* datagram) {
    return _datagram_wait_result(datagram, true);
}

int datagram_send(
    datagram_t* datagram,
    const void* data,
    int         len,
    const addr_t* to) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    _datagram_ref(datagram);
    int ret = -1;

    if (!atomic_load(&datagram->closed)) {
        for (;;) {
            bool again = false;
            int  n     = datagram_try_send(datagram, data, len, to, &again);
            if (n >= 0) {
                ret = 0;
                break;
            }
            if (!again || datagram_wait_write(datagram) != 0) {
                break;
            }
        }
    }

    _datagram_unref(datagram);
    return ret;
}

int datagram_local_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(datagram->fd, (struct sockaddr*)&addr.storage, &len)
        != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int datagram_remote_addr(
    datagram_t* datagram,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    if (!datagram->connected || _datagram_load_peer_addr(datagram) != 0) {
        return -1;
    }
    return addr_ntop(&datagram->peer_addr, host, host_len, port);
}

platform_sock_t datagram_fd(datagram_t* datagram) {
    return datagram->fd;
}
