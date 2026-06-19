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
#include "net/datagram.h"
#include "runtime/precond.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_udp_chan_s {
    datagram_t*     datagram;
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
    datagram_release(udp->datagram);
    free(udp);
}

xylem_udp_chan_t* xylem_udp_listen(const char* host, uint16_t port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_listen");

    datagram_t* datagram = datagram_listen(host, port);
    if (!datagram) {
        return NULL;
    }

    xylem_udp_chan_t* udp =
        (xylem_udp_chan_t*)calloc(1, sizeof(xylem_udp_chan_t));
    if (!udp) {
        datagram_release(datagram);
        return NULL;
    }

    udp->datagram = datagram;
    _udp_chan_ref(udp);
    return udp;
}

xylem_udp_chan_t* xylem_udp_dial(const char* host, uint16_t port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_dial");

    datagram_t* datagram = datagram_dial(host, port);
    if (!datagram) {
        return NULL;
    }

    xylem_udp_chan_t* udp =
        (xylem_udp_chan_t*)calloc(1, sizeof(xylem_udp_chan_t));
    if (!udp) {
        datagram_release(datagram);
        return NULL;
    }

    udp->datagram  = datagram;
    udp->connected = true;
    _udp_chan_ref(udp);
    return udp;
}

int xylem_udp_recv(
    xylem_udp_chan_t* udp,
    void*        buf,
    int          len,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_recv");

    if (!buf || len <= 0) {
        return -1;
    }

    _udp_chan_ref(udp);

    int ret = -1;
    if (!atomic_load_explicit(&udp->closed, memory_order_acquire)) {
        addr_t from;
        addr_t* from_ptr = (host || port) ? &from : NULL;
        ret = datagram_recv(udp->datagram, buf, len, from_ptr);
        if (ret >= 0 && from_ptr) {
            (void)addr_ntop(from_ptr, host, host_len, port);
        }
    }

    _udp_chan_unref(udp);
    return ret;
}

int xylem_udp_send(
    xylem_udp_chan_t* udp,
    const void*  data,
    int          len,
    const char*  host,
    uint16_t     port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_send");

    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    _udp_chan_ref(udp);

    int ret = -1;
    if (!atomic_load_explicit(&udp->closed, memory_order_acquire)) {
        const addr_t* to = NULL;
        addr_t        dest;
        if (host && !udp->connected) {
            if (addr_pton(host, port, &dest) != 0) {
                xylem_loge("<udp> send needs numeric ip host=%s", host);
                _udp_chan_unref(udp);
                return -1;
            } else {
                to = &dest;
            }
        }
        ret = datagram_send(udp->datagram, data, len, to);
    }

    _udp_chan_unref(udp);
    return ret;
}

void xylem_udp_set_read_deadline(xylem_udp_chan_t* udp, uint64_t deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_set_read_deadline");

    _udp_chan_ref(udp);
    if (!atomic_load_explicit(&udp->closed, memory_order_acquire)) {
        datagram_set_read_deadline(udp->datagram, deadline_ms);
    }
    _udp_chan_unref(udp);
}

void xylem_udp_set_write_deadline(xylem_udp_chan_t* udp, uint64_t deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_set_write_deadline");

    _udp_chan_ref(udp);
    if (!atomic_load_explicit(&udp->closed, memory_order_acquire)) {
        datagram_set_write_deadline(udp->datagram, deadline_ms);
    }
    _udp_chan_unref(udp);
}

void xylem_udp_close(xylem_udp_chan_t* udp) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_close");

    if (atomic_exchange(&udp->closed, true)) {
        return;
    }
    datagram_interrupt(udp->datagram);
    _udp_chan_unref(udp);
}

int xylem_udp_local_addr(
    xylem_udp_chan_t* udp,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_local_addr");

    _udp_chan_ref(udp);
    int ret = -1;
    if (!atomic_load_explicit(&udp->closed, memory_order_acquire)) {
        ret = datagram_local_addr(udp->datagram, host, host_len, port);
    }
    _udp_chan_unref(udp);
    return ret;
}

int xylem_udp_remote_addr(
    xylem_udp_chan_t* udp,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_remote_addr");

    _udp_chan_ref(udp);
    int ret = -1;
    if (!atomic_load_explicit(&udp->closed, memory_order_acquire)
        && udp->connected) {
        ret = datagram_remote_addr(udp->datagram, host, host_len, port);
    }
    _udp_chan_unref(udp);
    return ret;
}
