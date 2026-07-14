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
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_udp_chan_s {
    datagram_t*  datagram;
    bool         connected;
    _Atomic bool closed;
};

static xylem_udp_chan_t* _udp_chan_create(
    datagram_t* datagram,
    bool        connected) {
    xylem_udp_chan_t* udp
        = (xylem_udp_chan_t*)calloc(1, sizeof(xylem_udp_chan_t));
    if (!udp) {
        datagram_destroy(datagram);
        return NULL;
    }

    udp->datagram  = datagram;
    udp->connected = connected;
    atomic_init(&udp->closed, false);
    return udp;
}

static void _udp_consume_io_credit(void) {
    if (runtime_consume_credit(RUNTIME_IO_CREDIT_COST)) {
        runtime_yield();
    }
}

xylem_udp_chan_t* xylem_udp_listen(const char* host, uint16_t port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_listen");

    datagram_t* datagram = datagram_listen(host, port);
    if (!datagram) {
        return NULL;
    }

    return _udp_chan_create(datagram, false);
}

xylem_udp_chan_t* xylem_udp_dial(const char* host, uint16_t port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_dial");

    datagram_t* datagram = datagram_dial(host, port);
    if (!datagram) {
        return NULL;
    }

    return _udp_chan_create(datagram, true);
}

void xylem_udp_set_read_deadline(
    xylem_udp_chan_t* udp,
    uint64_t          deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_set_read_deadline");

    if (!atomic_load(&udp->closed)) {
        datagram_set_read_deadline(udp->datagram, deadline_ms);
    }
}

void xylem_udp_set_write_deadline(
    xylem_udp_chan_t* udp,
    uint64_t          deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_set_write_deadline");

    if (!atomic_load(&udp->closed)) {
        datagram_set_write_deadline(udp->datagram, deadline_ms);
    }
}

int xylem_udp_recv(
    xylem_udp_chan_t* udp,
    void*             buf,
    int               len,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_recv");

    if (!buf || len <= 0) {
        return -1;
    }

    addr_t  from;
    addr_t* from_ptr = (host || port) ? &from : NULL;

    for (;;) {
        if (atomic_load(&udp->closed)) {
            return -1;
        }

        int n = datagram_recv(udp->datagram, buf, len, from_ptr);
        if (n >= 0) {
            _udp_consume_io_credit();
            if (from_ptr) {
                (void)addr_ntop(from_ptr, host, host_len, port);
            }
            return n;
        }
        if (n != DATAGRAM_IO_AGAIN
            || datagram_wait_read(udp->datagram) != IOWAIT_READY) {
            return -1;
        }
    }
}

int xylem_udp_send(
    xylem_udp_chan_t* udp,
    const void*       data,
    int               len,
    const char*       host,
    uint16_t          port) {
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
    if ((udp->connected && host) || (!udp->connected && !host)) {
        return -1;
    }

    addr_t dest;
    if (!udp->connected && addr_pton(host, port, &dest) != 0) {
        xylem_loge("<udp> send needs numeric ip host=%s", host);
        return -1;
    }

    for (;;) {
        if (atomic_load(&udp->closed)) {
            return -1;
        }

        int n = datagram_send(
            udp->datagram,
            data,
            len,
            udp->connected ? NULL : &dest);
        if (n >= 0) {
            _udp_consume_io_credit();
            return 0;
        }
        if (n != DATAGRAM_IO_AGAIN
            || datagram_wait_write(udp->datagram) != IOWAIT_READY) {
            return -1;
        }
    }
}

void xylem_udp_close(xylem_udp_chan_t* udp) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_close");

    if (!atomic_exchange(&udp->closed, true)) {
        datagram_close(udp->datagram);
    }
}

void xylem_udp_destroy(xylem_udp_chan_t* udp) {
    if (!udp) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_destroy");

    xylem_udp_close(udp);
    datagram_destroy(udp->datagram);
    free(udp);
}

int xylem_udp_remote_addr(
    xylem_udp_chan_t* udp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_remote_addr");

    int ret = -1;
    if (!atomic_load(&udp->closed)
        && udp->connected) {
        ret = datagram_remote_addr(udp->datagram, host, host_len, port);
    }
    return ret;
}

int xylem_udp_local_addr(
    xylem_udp_chan_t* udp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    RUNTIME_REQUIRE_COROUTINE("udp", "xylem_udp_local_addr");

    int ret = -1;
    if (!atomic_load(&udp->closed)) {
        ret = datagram_local_addr(udp->datagram, host, host_len, port);
    }
    return ret;
}
