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
#include "runtime/runtime.h"
#include "minicoro/minicoro.h"
#include "mpsc.h"

#include <stdlib.h>
#include <stdbool.h>

typedef struct _ch_msg_s {
    mpsc_node_t node;
    void*       payload;
} _ch_msg_t;

struct xylem_channel_s {
    mpsc_t        queue;
    mco_coro*     wait_coro;
    bool          closed;
};

static void _channel_wakeup_cb(loop_t* loop, loop_post_t* req,
                               void* ud) {
    (void)loop; (void)req;
    xylem_channel_t* ch = (xylem_channel_t*)ud;
    if (ch->wait_coro) {
        mco_coro* co = ch->wait_coro;
        ch->wait_coro = NULL;
        mco_resume(co);
    }
}

xylem_channel_t* xylem_channel_create(void) {
    xylem_channel_t* ch = (xylem_channel_t*)calloc(1, sizeof(xylem_channel_t));
    if (!ch) return NULL;
    mpsc_init(&ch->queue);
    return ch;
}

void xylem_channel_destroy(xylem_channel_t* ch) {
    if (!ch) return;
    ch->closed = true;

    /* Wake blocked receiver so it returns NULL. */
    if (ch->wait_coro) {
        mco_coro* co = ch->wait_coro;
        ch->wait_coro = NULL;
        mco_resume(co);
    }

    /* Drain remaining messages. */
    mpsc_node_t* node;
    while ((node = mpsc_pop(&ch->queue)) != NULL) {
        _ch_msg_t* msg = mpsc_entry(node, _ch_msg_t, node);
        free(msg);
    }
    free(ch);
}

int xylem_channel_send(xylem_channel_t* ch, void* msg) {
    if (!ch || !msg) return -1;

    _ch_msg_t* m = (_ch_msg_t*)calloc(1, sizeof(_ch_msg_t));
    if (!m) return -1;

    m->payload = msg;
    mpsc_push(&ch->queue, &m->node);

    loop_post(runtime_loop(), _channel_wakeup_cb, ch);
    return 0;
}

void* xylem_channel_recv(xylem_channel_t* ch) {
    if (!ch) return NULL;

    for (;;) {
        if (ch->closed) return NULL;

        mpsc_node_t* node = mpsc_pop(&ch->queue);
        if (node) {
            _ch_msg_t* m = mpsc_entry(node, _ch_msg_t, node);
            void* payload = m->payload;
            free(m);
            return payload;
        }

        /* Queue empty: suspend coroutine until send wakes us. */
        ch->wait_coro = mco_running();
        mco_yield(mco_running());
    }
}
