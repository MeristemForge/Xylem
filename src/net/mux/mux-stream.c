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

#include "mux-stream.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdlib.h>
#include <string.h>

struct xylem_mux_stream_s* mux_stream_create(
    xylem_mux_t* mux, uint32_t id, uint32_t window) {
    struct xylem_mux_stream_s* s = (struct xylem_mux_stream_s*)calloc(
        1, sizeof(struct xylem_mux_stream_s));
    if (!s) {
        return NULL;
    }

    s->mux         = mux;
    s->id          = id;
    s->state       = MUX_STREAM_INIT;
    s->recv_window = window;
    s->send_window = window;
    s->recv_cap    = window;
    s->recv_buf    = (uint8_t*)calloc(1, s->recv_cap);
    if (!s->recv_buf) {
        free(s);
        return NULL;
    }
    spin_init(&s->lock);
    atomic_store(&s->refcnt, 1);
    return s;
}

void mux_stream_ref(struct xylem_mux_stream_s* s) {
    atomic_fetch_add(&s->refcnt, 1);
}

void mux_stream_unref(struct xylem_mux_stream_s* s) {
    if (atomic_fetch_sub(&s->refcnt, 1) != 1) {
        return;
    }
    free(s->recv_buf);
    free(s);
}

static void _mux_stream_wake_recv(struct xylem_mux_stream_s* s) {
    mco_coro* co = atomic_exchange(&s->recv_park, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _mux_stream_wake_send(struct xylem_mux_stream_s* s) {
    mco_coro* co = atomic_exchange(&s->send_park, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

void mux_stream_push_data(
    struct xylem_mux_stream_s* s, const void* data, size_t len) {
    if (len == 0) {
        return;
    }
    spin_lock(&s->lock);
    if (s->recv_len + len > s->recv_cap) {
        size_t new_cap = s->recv_cap * 2;
        while (new_cap < s->recv_len + len) {
            new_cap *= 2;
        }
        uint8_t* buf = (uint8_t*)realloc(s->recv_buf, new_cap);
        if (!buf) {
            spin_unlock(&s->lock);
            return;
        }
        s->recv_buf = buf;
        s->recv_cap = new_cap;
    }
    memcpy(s->recv_buf + s->recv_len, data, len);
    s->recv_len += len;
    s->recv_window -= (uint32_t)len;
    spin_unlock(&s->lock);
    _mux_stream_wake_recv(s);
}

void mux_stream_update_send_window(
    struct xylem_mux_stream_s* s, uint32_t delta) {
    spin_lock(&s->lock);
    s->send_window += delta;
    spin_unlock(&s->lock);
    _mux_stream_wake_send(s);
}

void mux_stream_notify_remote_fin(struct xylem_mux_stream_s* s) {
    spin_lock(&s->lock);
    if (s->state == MUX_STREAM_ESTABLISHED) {
        s->state = MUX_STREAM_REMOTE_CLOSE;
    } else if (s->state == MUX_STREAM_LOCAL_CLOSE) {
        s->state = MUX_STREAM_CLOSED;
    }
    spin_unlock(&s->lock);
    _mux_stream_wake_recv(s);
}

void mux_stream_notify_reset(struct xylem_mux_stream_s* s) {
    spin_lock(&s->lock);
    s->state = MUX_STREAM_CLOSED;
    spin_unlock(&s->lock);
    atomic_store(&s->closed, true);
    _mux_stream_wake_recv(s);
    _mux_stream_wake_send(s);
}
