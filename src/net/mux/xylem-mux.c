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

#include "xylem/net/xylem-mux.h"

#include "xylem/net/xylem-tcp.h"
#include "xylem/net/xylem-tls.h"
#include "xylem/net/xylem-uds.h"
#include "xylem/xylem-logger.h"

#include "mux-frame.h"
#include "mux-stream.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/spin.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MUX_MAX_FRAME_PAYLOAD 65535

typedef int (*_mux_read_fn_t)(void* ctx, void* buf, int len);
typedef int (*_mux_write_fn_t)(void* ctx, const void* data, int len);

struct xylem_mux_s {
    void*                       transport_ctx;
    _mux_read_fn_t              read_fn;
    _mux_write_fn_t             write_fn;
    xylem_mux_role_t            role;
    uint32_t                    next_stream_id;
    xylem_channel_t*            accept_ch;
    xylem_mutex_t*              write_mu;
    struct xylem_mux_stream_s** streams;
    size_t                      stream_count;
    size_t                      stream_cap;
    /**
     * `streams_lock` guards the streams[] array (streams, stream_count,
     * stream_cap) and next_stream_id against concurrent access by the
     * reader coroutine (accept-via-SYN) and user coroutines
     * (open_stream). Streams are never removed from the array until the
     * mux is freed, and each array slot holds a stream reference, so a
     * pointer returned by _mux_find_stream stays valid for the mux's
     * lifetime. Never held across a transport write or a park.
     */
    spin_t                      streams_lock;
    uint32_t                    max_stream_window;
    _Atomic bool                closed;
    _Atomic int32_t             refcnt;
};

static void _mux_ref(xylem_mux_t* mux) {
    atomic_fetch_add_explicit(&mux->refcnt, 1, memory_order_relaxed);
}

static void _mux_unref(xylem_mux_t* mux) {
    if (atomic_fetch_sub_explicit(&mux->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    for (size_t i = 0; i < mux->stream_count; i++) {
        if (mux->streams[i]) {
            mux_stream_unref(mux->streams[i]);
        }
    }
    free(mux->streams);
    xylem_channel_destroy(mux->accept_ch);
    xylem_mutex_destroy(mux->write_mu);
    free(mux);
}

static struct xylem_mux_stream_s* _mux_find_stream(
    xylem_mux_t* mux, uint32_t id) {
    /* Caller must hold mux->streams_lock. */
    for (size_t i = 0; i < mux->stream_count; i++) {
        if (mux->streams[i] && mux->streams[i]->id == id) {
            return mux->streams[i];
        }
    }
    return NULL;
}

static int _mux_add_stream(
    xylem_mux_t* mux, struct xylem_mux_stream_s* s) {
    /* Caller must hold mux->streams_lock. */
    if (mux->stream_count == mux->stream_cap) {
        size_t new_cap = mux->stream_cap == 0 ? 16 : mux->stream_cap * 2;
        struct xylem_mux_stream_s** arr =
            (struct xylem_mux_stream_s**)realloc(
                mux->streams,
                new_cap * sizeof(struct xylem_mux_stream_s*));
        if (!arr) {
            return -1;
        }
        mux->streams    = arr;
        mux->stream_cap = new_cap;
    }
    mux->streams[mux->stream_count++] = s;
    return 0;
}

static int _mux_read_full(xylem_mux_t* mux, void* buf, int len) {
    uint8_t* ptr = (uint8_t*)buf;
    int      rem = len;
    while (rem > 0) {
        int n = mux->read_fn(mux->transport_ctx, ptr, rem);
        if (n <= 0) {
            return -1;
        }
        ptr += n;
        rem -= n;
    }
    return 0;
}

static int _mux_write_frame(
    xylem_mux_t* mux, _mux_frame_hdr_t* hdr, const void* data) {
    uint8_t buf[MUX_FRAME_HDR_SIZE];
    mux_frame_encode(hdr, buf);

    xylem_mutex_lock(mux->write_mu);
    int rc = mux->write_fn(mux->transport_ctx, buf, MUX_FRAME_HDR_SIZE);
    if (rc == 0 && data && hdr->length > 0) {
        rc = mux->write_fn(mux->transport_ctx, data, hdr->length);
    }
    xylem_mutex_unlock(mux->write_mu);
    return rc;
}

static void _mux_send_window_update(
    xylem_mux_t* mux, uint32_t stream_id, uint32_t delta) {
    _mux_frame_hdr_t hdr = {
        .version   = MUX_PROTO_VERSION,
        .type      = MUX_TYPE_WINDOW_UPDATE,
        .flags     = 0,
        .stream_id = stream_id,
        .length    = delta
    };
    _mux_write_frame(mux, &hdr, NULL);
}

static struct xylem_mux_stream_s* _mux_accept_syn(
    xylem_mux_t* mux, uint32_t stream_id) {
    bool created = false;

    spin_lock(&mux->streams_lock);
    struct xylem_mux_stream_s* s = _mux_find_stream(mux, stream_id);
    if (!s) {
        s = mux_stream_create(mux, stream_id, mux->max_stream_window);
        if (s) {
            s->state = MUX_STREAM_ESTABLISHED;
            mux_stream_ref(s);
            _mux_add_stream(mux, s);
            created = true;
        }
    }
    spin_unlock(&mux->streams_lock);

    /* Hand the new stream to a parked accept coroutine outside the
     * spin: xylem_channel_send may touch scheduler state and must not
     * run under a spin lock. */
    if (created) {
        xylem_channel_send(mux->accept_ch, s);
    }
    return s;
}

static int _mux_discard_payload(xylem_mux_t* mux, uint32_t length) {
    uint8_t discard[4096];
    int rem = (int)length;
    while (rem > 0) {
        int chunk = rem < (int)sizeof(discard) ? rem : (int)sizeof(discard);
        if (_mux_read_full(mux, discard, chunk) != 0) {
            return -1;
        }
        rem -= chunk;
    }
    return 0;
}

static int _mux_handle_data(xylem_mux_t* mux, _mux_frame_hdr_t* hdr) {
    spin_lock(&mux->streams_lock);
    struct xylem_mux_stream_s* s = _mux_find_stream(mux, hdr->stream_id);
    spin_unlock(&mux->streams_lock);

    if (hdr->flags & MUX_FLAG_SYN) {
        s = _mux_accept_syn(mux, hdr->stream_id);
    }

    if (s && hdr->length > 0) {
        uint8_t* payload = (uint8_t*)malloc(hdr->length);
        if (payload && _mux_read_full(mux, payload, (int)hdr->length) == 0) {
            mux_stream_push_data(s, payload, hdr->length);
        }
        free(payload);
    } else if (hdr->length > 0) {
        if (_mux_discard_payload(mux, hdr->length) != 0) {
            return -1;
        }
    }

    if (s && (hdr->flags & MUX_FLAG_FIN)) {
        mux_stream_notify_remote_fin(s);
    }
    if (s && (hdr->flags & MUX_FLAG_RST)) {
        mux_stream_notify_reset(s);
    }
    return 0;
}

static void _mux_handle_window_update(
    xylem_mux_t* mux, _mux_frame_hdr_t* hdr) {
    if (hdr->flags & MUX_FLAG_SYN) {
        _mux_accept_syn(mux, hdr->stream_id);
    }
    spin_lock(&mux->streams_lock);
    struct xylem_mux_stream_s* s = _mux_find_stream(mux, hdr->stream_id);
    spin_unlock(&mux->streams_lock);
    if (s) {
        mux_stream_update_send_window(s, hdr->length);
    }
}

static void _mux_handle_ping(xylem_mux_t* mux, _mux_frame_hdr_t* hdr) {
    if (!(hdr->flags & MUX_FLAG_ACK)) {
        _mux_frame_hdr_t pong = {
            .version   = MUX_PROTO_VERSION,
            .type      = MUX_TYPE_PING,
            .flags     = MUX_FLAG_ACK,
            .stream_id = 0,
            .length    = hdr->length
        };
        _mux_write_frame(mux, &pong, NULL);
    }
}

static void _mux_teardown(xylem_mux_t* mux) {
    atomic_store_explicit(&mux->closed, true, memory_order_release);

    spin_lock(&mux->streams_lock);
    size_t n = mux->stream_count;
    spin_unlock(&mux->streams_lock);
    for (size_t i = 0; i < n; i++) {
        spin_lock(&mux->streams_lock);
        struct xylem_mux_stream_s* s = mux->streams[i];
        spin_unlock(&mux->streams_lock);
        if (s) {
            mux_stream_notify_reset(s);
        }
    }

    xylem_channel_destroy(mux->accept_ch);
    mux->accept_ch = NULL;
    _mux_unref(mux);
}

static int _mux_dispatch(xylem_mux_t* mux, _mux_frame_hdr_t* hdr) {
    switch (hdr->type) {
    case MUX_TYPE_DATA:
        return _mux_handle_data(mux, hdr);
    case MUX_TYPE_WINDOW_UPDATE:
        _mux_handle_window_update(mux, hdr);
        return 0;
    case MUX_TYPE_PING:
        _mux_handle_ping(mux, hdr);
        return 0;
    case MUX_TYPE_GO_AWAY:
        atomic_store_explicit(&mux->closed, true, memory_order_release);
        return 0;
    default:
        return 0;
    }
}

static void _mux_reader_loop(void* arg) {
    xylem_mux_t* mux = (xylem_mux_t*)arg;
    uint8_t hdr_buf[MUX_FRAME_HDR_SIZE];

    while (!atomic_load_explicit(&mux->closed, memory_order_acquire)) {
        if (_mux_read_full(mux, hdr_buf, MUX_FRAME_HDR_SIZE) != 0) {
            break;
        }

        _mux_frame_hdr_t hdr;
        mux_frame_decode(hdr_buf, &hdr);

        if (hdr.version != MUX_PROTO_VERSION) {
            xylem_loge("mux: unsupported protocol version %d", hdr.version);
            break;
        }

        if (_mux_dispatch(mux, &hdr) != 0) {
            break;
        }
    }

    _mux_teardown(mux);
}

static int _mux_resolve_transport(
    xylem_mux_transport_t transport,
    _mux_read_fn_t*         out_read,
    _mux_write_fn_t*        out_write) {
    switch (transport) {
    case XYLEM_MUX_TCP:
        *out_read  = (_mux_read_fn_t)xylem_tcp_read;
        *out_write = (_mux_write_fn_t)xylem_tcp_write;
        return 0;
    case XYLEM_MUX_TLS:
        *out_read  = (_mux_read_fn_t)xylem_tls_read;
        *out_write = (_mux_write_fn_t)xylem_tls_write;
        return 0;
    case XYLEM_MUX_UDS:
        *out_read  = (_mux_read_fn_t)xylem_uds_read;
        *out_write = (_mux_write_fn_t)xylem_uds_write;
        return 0;
    case XYLEM_MUX_RUDP_STREAM:
        xylem_loge("RUDP stream transport not yet implemented for mux");
        return -1;
    default:
        xylem_loge("unsupported transport for mux");
        return -1;
    }
}

xylem_mux_t* xylem_mux_create(
    void* conn,
    xylem_mux_transport_t transport,
    xylem_mux_role_t role,
    xylem_mux_opts_t* opts) {
    _mux_read_fn_t  read_fn;
    _mux_write_fn_t write_fn;
    if (_mux_resolve_transport(transport, &read_fn, &write_fn) != 0) {
        return NULL;
    }

    xylem_mux_t* mux = (xylem_mux_t*)calloc(1, sizeof(xylem_mux_t));
    if (!mux) {
        return NULL;
    }

    mux->transport_ctx = conn;
    mux->read_fn       = read_fn;
    mux->write_fn      = write_fn;
    mux->role          = role;
    mux->next_stream_id = (role == XYLEM_MUX_CLIENT) ? 1 : 2;

    mux->max_stream_window = MUX_DEFAULT_WINDOW;
    if (opts && opts->max_stream_window > 0) {
        mux->max_stream_window = opts->max_stream_window;
    }

    spin_init(&mux->streams_lock);

    mux->accept_ch = xylem_channel_create();
    if (!mux->accept_ch) {
        free(mux);
        return NULL;
    }

    mux->write_mu = xylem_mutex_create();
    if (!mux->write_mu) {
        xylem_channel_destroy(mux->accept_ch);
        free(mux);
        return NULL;
    }

    atomic_store_explicit(&mux->refcnt, 1, memory_order_relaxed);

    _mux_ref(mux);
    runtime_spawn(_mux_reader_loop, mux);

    return mux;
}

void xylem_mux_destroy(xylem_mux_t* mux) {
    if (atomic_exchange(&mux->closed, true)) {
        return;
    }

    _mux_frame_hdr_t hdr = {
        .version   = MUX_PROTO_VERSION,
        .type      = MUX_TYPE_GO_AWAY,
        .flags     = 0,
        .stream_id = 0,
        .length    = 0
    };
    _mux_write_frame(mux, &hdr, NULL);

    spin_lock(&mux->streams_lock);
    size_t n = mux->stream_count;
    spin_unlock(&mux->streams_lock);
    for (size_t i = 0; i < n; i++) {
        spin_lock(&mux->streams_lock);
        struct xylem_mux_stream_s* s = mux->streams[i];
        spin_unlock(&mux->streams_lock);
        if (s) {
            mux_stream_notify_reset(s);
        }
    }
    _mux_unref(mux);
}

xylem_mux_stream_t* xylem_mux_open_stream(xylem_mux_t* mux) {
    if (atomic_load_explicit(&mux->closed, memory_order_acquire)) {
        return NULL;
    }

    spin_lock(&mux->streams_lock);
    uint32_t id = mux->next_stream_id;
    mux->next_stream_id += 2;
    spin_unlock(&mux->streams_lock);

    struct xylem_mux_stream_s* s =
        mux_stream_create(mux, id, mux->max_stream_window);
    if (!s) {
        return NULL;
    }
    s->state = MUX_STREAM_ESTABLISHED;

    _mux_frame_hdr_t hdr = {
        .version   = MUX_PROTO_VERSION,
        .type      = MUX_TYPE_WINDOW_UPDATE,
        .flags     = MUX_FLAG_SYN,
        .stream_id = id,
        .length    = mux->max_stream_window
    };
    if (_mux_write_frame(mux, &hdr, NULL) != 0) {
        mux_stream_unref(s);
        return NULL;
    }

    mux_stream_ref(s);
    spin_lock(&mux->streams_lock);
    _mux_add_stream(mux, s);
    spin_unlock(&mux->streams_lock);
    return s;
}

xylem_mux_stream_t* xylem_mux_accept_stream(xylem_mux_t* mux) {
    if (atomic_load_explicit(&mux->closed, memory_order_acquire)) {
        return NULL;
    }
    if (!mux->accept_ch) {
        return NULL;
    }
    return (xylem_mux_stream_t*)xylem_channel_recv(mux->accept_ch);
}

static bool _mux_recv_park_cb(mco_coro* co, void* arg) {
    struct xylem_mux_stream_s* s = (struct xylem_mux_stream_s*)arg;
    mco_coro* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &s->recv_park, &expected, co,
            memory_order_acq_rel, memory_order_acquire)) {
        return true;
    }
    return false;
}

static bool _mux_send_park_cb(mco_coro* co, void* arg) {
    struct xylem_mux_stream_s* s = (struct xylem_mux_stream_s*)arg;
    mco_coro* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &s->send_park, &expected, co,
            memory_order_acq_rel, memory_order_acquire)) {
        return true;
    }
    return false;
}

int xylem_mux_read(xylem_mux_stream_t* s, void* buf, int len) {
    mux_stream_ref(s);

    for (;;) {
        spin_lock(&s->lock);
        if (s->recv_len > 0) {
            int n = (int)s->recv_len < len ? (int)s->recv_len : len;
            memcpy(buf, s->recv_buf, (size_t)n);
            if ((size_t)n < s->recv_len) {
                memmove(s->recv_buf, s->recv_buf + n, s->recv_len - (size_t)n);
            }
            s->recv_len -= (size_t)n;

            uint32_t consumed = s->mux->max_stream_window
                                - s->recv_window - (uint32_t)s->recv_len;
            bool send_update = consumed >= s->mux->max_stream_window / 2;
            if (send_update) {
                s->recv_window += consumed;
            }
            spin_unlock(&s->lock);

            /* Transport write happens outside the stream spin. */
            if (send_update) {
                _mux_send_window_update(s->mux, s->id, consumed);
            }

            mux_stream_unref(s);
            return n;
        }

        _mux_stream_state_t st = s->state;
        spin_unlock(&s->lock);

        if (st == MUX_STREAM_REMOTE_CLOSE
            || st == MUX_STREAM_CLOSED
            || atomic_load_explicit(&s->closed, memory_order_acquire)) {
            mux_stream_unref(s);
            return (st == MUX_STREAM_CLOSED) ? -1 : 0;
        }

        scheduler_park(runtime_get_scheduler(), _mux_recv_park_cb, s);
    }
}

int xylem_mux_write(xylem_mux_stream_t* s, const void* data, int len) {
    mux_stream_ref(s);
    const uint8_t* ptr = (const uint8_t*)data;
    int            rem = len;

    while (rem > 0) {
        spin_lock(&s->lock);
        if (atomic_load_explicit(&s->closed, memory_order_acquire)
            || s->state == MUX_STREAM_CLOSED) {
            spin_unlock(&s->lock);
            mux_stream_unref(s);
            return -1;
        }

        uint32_t window = s->send_window;
        spin_unlock(&s->lock);

        if (window == 0) {
            scheduler_park(
                runtime_get_scheduler(), _mux_send_park_cb, s);
            continue;
        }

        uint32_t chunk = window;
        if (chunk > MUX_MAX_FRAME_PAYLOAD) {
            chunk = MUX_MAX_FRAME_PAYLOAD;
        }
        if (chunk > (uint32_t)rem) {
            chunk = (uint32_t)rem;
        }

        _mux_frame_hdr_t hdr = {
            .version   = MUX_PROTO_VERSION,
            .type      = MUX_TYPE_DATA,
            .flags     = 0,
            .stream_id = s->id,
            .length    = chunk
        };
        if (_mux_write_frame(s->mux, &hdr, ptr) != 0) {
            mux_stream_unref(s);
            return -1;
        }

        spin_lock(&s->lock);
        s->send_window -= chunk;
        spin_unlock(&s->lock);
        ptr += chunk;
        rem -= chunk;
    }

    mux_stream_unref(s);
    return 0;
}

void xylem_mux_close_stream(xylem_mux_stream_t* s) {
    if (atomic_exchange(&s->closed, true)) {
        return;
    }

    spin_lock(&s->lock);
    if (s->state == MUX_STREAM_ESTABLISHED) {
        s->state = MUX_STREAM_LOCAL_CLOSE;
    } else {
        s->state = MUX_STREAM_CLOSED;
    }
    spin_unlock(&s->lock);

    _mux_frame_hdr_t hdr = {
        .version   = MUX_PROTO_VERSION,
        .type      = MUX_TYPE_DATA,
        .flags     = MUX_FLAG_FIN,
        .stream_id = s->id,
        .length    = 0
    };
    _mux_write_frame(s->mux, &hdr, NULL);
    mux_stream_unref(s);
}

void xylem_mux_set_read_deadline(xylem_mux_stream_t* s, uint64_t deadline_ms) {
    s->rd_deadline = deadline_ms;
}

void xylem_mux_set_write_deadline(xylem_mux_stream_t* s, uint64_t deadline_ms) {
    s->wr_deadline = deadline_ms;
}
