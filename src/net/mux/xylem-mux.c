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

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MUX_MAX_FRAME_PAYLOAD 65535

typedef int64_t (*_mux_read_fn_t)(void* ctx, void* buf, size_t len);
typedef int (*_mux_write_fn_t)(void* ctx, const void* data, size_t len);

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
    for (size_t i = 0; i < mux->stream_count; i++) {
        if (mux->streams[i] && mux->streams[i]->id == id) {
            return mux->streams[i];
        }
    }
    return NULL;
}

static int _mux_add_stream(
    xylem_mux_t* mux, struct xylem_mux_stream_s* s) {
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

static int _mux_read_full(xylem_mux_t* mux, void* buf, size_t len) {
    uint8_t* ptr = (uint8_t*)buf;
    size_t   rem = len;
    while (rem > 0) {
        int64_t n = mux->read_fn(mux->transport_ctx, ptr, rem);
        if (n <= 0) {
            return -1;
        }
        ptr += n;
        rem -= (size_t)n;
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

        switch (hdr.type) {
        case MUX_TYPE_DATA: {
            struct xylem_mux_stream_s* s =
                _mux_find_stream(mux, hdr.stream_id);

            if (hdr.flags & MUX_FLAG_SYN) {
                if (!s) {
                    s = mux_stream_create(
                        mux, hdr.stream_id, mux->max_stream_window);
                    if (s) {
                        s->state = MUX_STREAM_ESTABLISHED;
                        mux_stream_ref(s);
                        _mux_add_stream(mux, s);
                        xylem_channel_send(mux->accept_ch, s);
                    }
                }
            }

            if (s && hdr.length > 0) {
                uint8_t* payload = (uint8_t*)malloc(hdr.length);
                if (payload
                    && _mux_read_full(mux, payload, hdr.length) == 0) {
                    mux_stream_push_data(s, payload, hdr.length);
                }
                free(payload);
            } else if (hdr.length > 0) {
                uint8_t discard[4096];
                size_t rem = hdr.length;
                while (rem > 0) {
                    size_t chunk = rem < sizeof(discard)
                                       ? rem : sizeof(discard);
                    if (_mux_read_full(mux, discard, chunk) != 0) {
                        goto exit_loop;
                    }
                    rem -= chunk;
                }
            }

            if (s && (hdr.flags & MUX_FLAG_FIN)) {
                mux_stream_notify_remote_fin(s);
            }
            if (s && (hdr.flags & MUX_FLAG_RST)) {
                mux_stream_notify_reset(s);
            }
            break;
        }
        case MUX_TYPE_WINDOW_UPDATE: {
            if (hdr.flags & MUX_FLAG_SYN) {
                struct xylem_mux_stream_s* s =
                    _mux_find_stream(mux, hdr.stream_id);
                if (!s) {
                    s = mux_stream_create(
                        mux, hdr.stream_id, mux->max_stream_window);
                    if (s) {
                        s->state = MUX_STREAM_ESTABLISHED;
                        mux_stream_ref(s);
                        _mux_add_stream(mux, s);
                        xylem_channel_send(mux->accept_ch, s);
                    }
                }
            }
            struct xylem_mux_stream_s* s =
                _mux_find_stream(mux, hdr.stream_id);
            if (s) {
                mux_stream_update_send_window(s, hdr.length);
            }
            break;
        }
        case MUX_TYPE_PING: {
            if (!(hdr.flags & MUX_FLAG_ACK)) {
                _mux_frame_hdr_t pong = {
                    .version   = MUX_PROTO_VERSION,
                    .type      = MUX_TYPE_PING,
                    .flags     = MUX_FLAG_ACK,
                    .stream_id = 0,
                    .length    = hdr.length
                };
                _mux_write_frame(mux, &pong, NULL);
            }
            break;
        }
        case MUX_TYPE_GO_AWAY: {
            atomic_store_explicit(&mux->closed, true, memory_order_release);
            break;
        }
        default:
            break;
        }
    }

exit_loop:
    atomic_store_explicit(&mux->closed, true, memory_order_release);
    for (size_t i = 0; i < mux->stream_count; i++) {
        if (mux->streams[i]) {
            mux_stream_notify_reset(mux->streams[i]);
        }
    }
    xylem_channel_destroy(mux->accept_ch);
    mux->accept_ch = NULL;
    _mux_unref(mux);
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
    xylem_spawn(_mux_reader_loop, mux);

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

    for (size_t i = 0; i < mux->stream_count; i++) {
        if (mux->streams[i]) {
            mux_stream_notify_reset(mux->streams[i]);
        }
    }
    _mux_unref(mux);
}

xylem_mux_stream_t* xylem_mux_open_stream(xylem_mux_t* mux) {
    if (atomic_load_explicit(&mux->closed, memory_order_acquire)) {
        return NULL;
    }

    uint32_t id = mux->next_stream_id;
    mux->next_stream_id += 2;

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
    _mux_add_stream(mux, s);
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

int64_t xylem_mux_read(xylem_mux_stream_t* s, void* buf, size_t len) {
    mux_stream_ref(s);

    for (;;) {
        if (s->recv_len > 0) {
            size_t n = s->recv_len < len ? s->recv_len : len;
            memcpy(buf, s->recv_buf, n);
            if (n < s->recv_len) {
                memmove(s->recv_buf, s->recv_buf + n, s->recv_len - n);
            }
            s->recv_len -= n;

            uint32_t consumed = s->mux->max_stream_window
                                - s->recv_window - (uint32_t)s->recv_len;
            if (consumed >= s->mux->max_stream_window / 2) {
                s->recv_window += consumed;
                _mux_send_window_update(s->mux, s->id, consumed);
            }

            mux_stream_unref(s);
            return (int64_t)n;
        }

        if (s->state == MUX_STREAM_REMOTE_CLOSE
            || s->state == MUX_STREAM_CLOSED
            || atomic_load_explicit(&s->closed, memory_order_acquire)) {
            mux_stream_unref(s);
            return (s->state == MUX_STREAM_CLOSED) ? -1 : 0;
        }

        scheduler_park(runtime_get_scheduler(), _mux_recv_park_cb, s);
    }
}

int xylem_mux_write(xylem_mux_stream_t* s, const void* data, size_t len) {
    mux_stream_ref(s);
    const uint8_t* ptr = (const uint8_t*)data;
    size_t         rem = len;

    while (rem > 0) {
        if (atomic_load_explicit(&s->closed, memory_order_acquire)
            || s->state == MUX_STREAM_CLOSED) {
            mux_stream_unref(s);
            return -1;
        }

        if (s->send_window == 0) {
            scheduler_park(
                runtime_get_scheduler(), _mux_send_park_cb, s);
            continue;
        }

        uint32_t chunk = s->send_window;
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

        s->send_window -= chunk;
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

    if (s->state == MUX_STREAM_ESTABLISHED) {
        s->state = MUX_STREAM_LOCAL_CLOSE;
    } else {
        s->state = MUX_STREAM_CLOSED;
    }

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
