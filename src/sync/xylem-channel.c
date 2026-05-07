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

#include "xylem/sync/xylem-channel.h"

#include "xylem/xylem-logger.h"

#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "container/mpsc.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct _channel_msg_s {
    mpsc_node_t node;
    void*       payload;
} _channel_msg_t;

struct xylem_channel_s {
    mpsc_t             queue;
    _Atomic(mco_coro*) wait_coro;
    _Atomic bool       closed;
};

xylem_channel_t* xylem_channel_create(void) {
    xylem_channel_t* ch =
        (xylem_channel_t*)calloc(1, sizeof(xylem_channel_t));
    if (!ch) {
        return NULL;
    }
    mpsc_init(&ch->queue);
    atomic_init(&ch->wait_coro, NULL);
    atomic_init(&ch->closed, false);
    return ch;
}

void xylem_channel_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }

    atomic_store(&ch->closed, true);

    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }

    mpsc_node_t* node;
    while ((node = mpsc_pop(&ch->queue)) != NULL) {
        _channel_msg_t* msg = mpsc_entry(node, _channel_msg_t, node);
        free(msg);
    }
    free(ch);
}

int xylem_channel_send(xylem_channel_t* ch, void* msg) {
    if (!ch || !msg) {
        return -1;
    }

    _channel_msg_t* m = (_channel_msg_t*)calloc(1, sizeof(_channel_msg_t));
    if (!m) {
        return -1;
    }

    m->payload = msg;
    mpsc_push(&ch->queue, &m->node);

    mco_coro* co = atomic_exchange(&ch->wait_coro, NULL);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
    return 0;
}

static bool _channel_park_cb(mco_coro* co, void* arg) {
    xylem_channel_t* ch = (xylem_channel_t*)arg;

    /**
     * Channel is MPSC by design: multiple senders, single receiver.
     * Publishing the waiter slot with exchange (not store) turns a
     * violated single-receiver contract into a loud failure instead
     * of a silent orphaned coroutine + leaked handle.
     *
     * Senders and destroy always exchange the slot back to NULL
     * before rescheduling the receiver, so on entry here the slot
     * must be NULL under the single-receiver contract.
     */
    mco_coro* prev = atomic_exchange(&ch->wait_coro, co);
    if (prev != NULL) {
        xylem_loge(
            "xylem_channel: concurrent recv violates "
            "single-receiver contract (ch=%p prev=%p new=%p); aborting",
            (void*)ch,
            (void*)prev,
            (void*)co);
        abort();
    }

    if (atomic_load(&ch->closed)) {
        mco_coro* expected = co;
        if (atomic_compare_exchange_strong(&ch->wait_coro, &expected, NULL)) {
            return false;
        }
        return true;
    }

    mpsc_node_t* node = mpsc_pop(&ch->queue);
    if (node) {
        mco_coro* expected = co;
        if (atomic_compare_exchange_strong(&ch->wait_coro, &expected, NULL)) {
            mpsc_push(&ch->queue, node);
            return false;
        }
        mpsc_push(&ch->queue, node);
        return true;
    }

    return true;
}

void* xylem_channel_recv(xylem_channel_t* ch) {
    if (!ch) {
        return NULL;
    }

    for (;;) {
        if (atomic_load(&ch->closed)) {
            return NULL;
        }

        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            _channel_msg_t* m = mpsc_entry(node, _channel_msg_t, node);
            void* payload = m->payload;
            free(m);
            return payload;
        }

        scheduler_park(runtime_get_scheduler(), _channel_park_cb, ch);
    }
}
