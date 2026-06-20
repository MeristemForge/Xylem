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

#include "xylem.h"
#include "assert.h"
#include "utils.h"

#include "container/mpsc.h"
#include "xylem/xylem-threads.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define SAFETY_TIMEOUT_MS 5000
#define TEST_CHANNEL_SEND_GATE_CLOSED (1u << 31)

static xylem_opts_t _rt_opts = { .workers = 4 };

#define CH_SENDERS  20
#define CH_MESSAGES 10

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    atomic_int       recv_count;
    int              tested;
} _ch_ctx_t;

static void _ch_sender(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    for (int i = 0; i < CH_MESSAGES; i++) {
        xylem_channel_send(ctx->ch, &ctx->payload);
    }
}

static void _ch_receiver(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    int total = CH_SENDERS * CH_MESSAGES;
    for (int i = 0; i < total; i++) {
        void* msg = xylem_channel_recv(ctx->ch);
        if (!msg) {
            break;
        }
        atomic_fetch_add(&ctx->recv_count, 1);
    }
    ASSERT(atomic_load(&ctx->recv_count) == total);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _test_ch_main(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create(0);
    xylem_spawn(_ch_receiver, ctx);
    for (int i = 0; i < CH_SENDERS; i++) {
        xylem_spawn(_ch_sender, ctx);
    }
}

static void test_concurrent(void) {
    fprintf(stderr, "=== test_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _ch_ctx_t ctx = {0};
        xylem_run(_test_ch_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    int              tested;
} _to_ctx_t;

typedef struct {
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;
    int*               payload;
    uint64_t           delay_ms;
} _stale_send_req_t;

static void _to_basic_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, 30);
    ASSERT(msg == NULL);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _to_basic_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create(0);
    xylem_spawn(_to_basic_coro, ctx);
}

static void test_timeout_empty(void) {
    fprintf(stderr, "=== test_timeout_empty\n");
    _to_ctx_t ctx = {0};
    xylem_run(_to_basic_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

static void _to_sender_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    xylem_sleep(20);
    xylem_channel_send(ctx->ch, &ctx->payload);
}

static void _to_recv_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, 1000);
    ASSERT(msg == &ctx->payload);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _to_deliver_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create(0);
    xylem_spawn(_to_recv_coro, ctx);
    xylem_spawn(_to_sender_coro, ctx);
}

static void test_timeout_deliver(void) {
    fprintf(stderr, "=== test_timeout_deliver\n");
    _to_ctx_t ctx = {0};
    xylem_run(_to_deliver_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

#define STALE_TIMER_ROUNDS 500

typedef struct {
    int tested;
} _stale_ctx_t;

static void _stale_send_after_coro(void* arg) {
    _stale_send_req_t* req = (_stale_send_req_t*)arg;
    if (req->delay_ms > 0) {
        xylem_sleep(req->delay_ms);
    }
    xylem_channel_send(req->ch, req->payload);
    xylem_waitgroup_done(req->wg);
}

static void _stale_recv_coro(void* arg) {
    _stale_ctx_t* ctx = (_stale_ctx_t*)arg;
    xylem_channel_t* ch = xylem_channel_create(0);
    xylem_waitgroup_t* wg = xylem_waitgroup_create();

    for (int round = 0; round < STALE_TIMER_ROUNDS; round++) {
        int first = round * 2;
        _stale_send_req_t first_req = {
            .ch = ch, .wg = wg, .payload = &first, .delay_ms = 1
        };
        xylem_waitgroup_add(wg, 1);
        xylem_spawn(_stale_send_after_coro, &first_req);
        void* first_msg = xylem_channel_recv_timeout(ch, 1);
        xylem_waitgroup_wait(wg);
        if (!first_msg) {
            first_msg = xylem_channel_recv_timeout(ch, 0);
        }
        ASSERT(first_msg == &first);

        int second = round * 2 + 1;
        _stale_send_req_t second_req = {
            .ch = ch, .wg = wg, .payload = &second, .delay_ms = 5
        };
        xylem_waitgroup_add(wg, 1);
        xylem_spawn(_stale_send_after_coro, &second_req);
        void* second_msg = xylem_channel_recv_timeout(ch, 1000);
        xylem_waitgroup_wait(wg);
        ASSERT(second_msg == &second);
    }

    ctx->tested = 1;
    xylem_waitgroup_destroy(wg);
    xylem_channel_destroy(ch);
    xylem_shutdown();
}

static void _stale_main(void* arg) {
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    xylem_spawn(_stale_recv_coro, arg);
}

static void test_timeout_stale_timer_does_not_end_next_recv(void) {
    fprintf(stderr, "=== test_timeout_stale_timer_does_not_end_next_recv\n");
    _stale_ctx_t ctx = {0};
    xylem_run(_stale_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

#define TO_RACE_ROUNDS 200

typedef struct {
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;
    int                payload;
    uint64_t           timeout_ms;
    uint64_t           send_at_ms;
    int                got_timeout;
    int                got_message;
} _race_ctx_t;

static void _race_sender_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (ctx->send_at_ms > now) {
        xylem_sleep(ctx->send_at_ms - now);
    }
    xylem_channel_send(ctx->ch, &ctx->payload);
    xylem_waitgroup_done(ctx->wg);
}

static void _race_recv_coro(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, ctx->timeout_ms);
    if (msg) {
        ASSERT(msg == &ctx->payload);
        ctx->got_message = 1;
    } else {
        ctx->got_timeout = 1;
    }

    xylem_waitgroup_wait(ctx->wg);
    if (ctx->got_timeout) {
        void* leftover = xylem_channel_recv(ctx->ch);
        ASSERT(leftover == &ctx->payload);
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    xylem_shutdown();
}

static void _race_main(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create(0);
    ctx->wg = xylem_waitgroup_create();
    ctx->timeout_ms = 5;
    ctx->send_at_ms =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 5;
    xylem_waitgroup_add(ctx->wg, 1);
    xylem_spawn(_race_recv_coro, ctx);
    xylem_spawn(_race_sender_coro, ctx);
}

static void test_timeout_race(void) {
    fprintf(stderr, "=== test_timeout_race\n");
    for (int round = 0; round < TO_RACE_ROUNDS; round++) {
        _race_ctx_t ctx = {0};
        ctx.payload = round;
        xylem_run(_race_main, &ctx, &_rt_opts);

        ASSERT(ctx.got_message ^ ctx.got_timeout);
    }
}

#define TR_SENDERS  8
#define TR_MESSAGES 50

typedef struct {
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;
    int                payload;
    atomic_int         recv_count;
    thrd_t             thr;
    int                tested;
} _tr_ctx_t;

static int _tr_recv_thread(void* arg) {
    _tr_ctx_t* ctx = (_tr_ctx_t*)arg;
    for (;;) {
        void* msg = xylem_channel_recv(ctx->ch);
        if (!msg) {
            break;
        }
        ASSERT(msg == &ctx->payload);
        atomic_fetch_add(&ctx->recv_count, 1);
    }
    return 0;
}

static void _tr_sender(void* arg) {
    _tr_ctx_t* ctx = (_tr_ctx_t*)arg;
    for (int i = 0; i < TR_MESSAGES; i++) {
        xylem_channel_send(ctx->ch, &ctx->payload);
    }
    xylem_waitgroup_done(ctx->wg);
}

static void _tr_coordinator(void* arg) {
    _tr_ctx_t* ctx = (_tr_ctx_t*)arg;
    xylem_waitgroup_wait(ctx->wg);
    xylem_channel_close(ctx->ch);
    thrd_join(ctx->thr, NULL);
    ASSERT(atomic_load(&ctx->recv_count) == TR_SENDERS * TR_MESSAGES);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    xylem_shutdown();
}

static void _tr_main(void* arg) {
    _tr_ctx_t* ctx = (_tr_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create(0);
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, TR_SENDERS);
    ASSERT(thrd_create(&ctx->thr, _tr_recv_thread, ctx) == thrd_success);
    for (int i = 0; i < TR_SENDERS; i++) {
        xylem_spawn(_tr_sender, ctx);
    }
    xylem_spawn(_tr_coordinator, ctx);
}

static void test_thread_recv(void) {
    fprintf(stderr, "=== test_thread_recv\n");
    for (int round = 0; round < 10; round++) {
        _tr_ctx_t ctx = {0};
        xylem_run(_tr_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    thrd_t           thr;
    int              tested;
} _ts_ctx_t;

static int _ts_send_thread(void* arg) {
    _ts_ctx_t* ctx = (_ts_ctx_t*)arg;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 30 * 1000 * 1000 };
    thrd_sleep(&ts, NULL);
    xylem_channel_send(ctx->ch, &ctx->payload);
    return 0;
}

static void _ts_recv_coro(void* arg) {
    _ts_ctx_t* ctx = (_ts_ctx_t*)arg;
    void* msg = xylem_channel_recv(ctx->ch);
    ASSERT(msg == &ctx->payload);
    thrd_join(ctx->thr, NULL);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_shutdown();
}

static void _ts_main(void* arg) {
    _ts_ctx_t* ctx = (_ts_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    ctx->ch = xylem_channel_create(0);
    ASSERT(thrd_create(&ctx->thr, _ts_send_thread, ctx) == thrd_success);
    xylem_spawn(_ts_recv_coro, ctx);
}

static void test_thread_send(void) {
    fprintf(stderr, "=== test_thread_send\n");
    for (int round = 0; round < 20; round++) {
        _ts_ctx_t ctx = {0};
        ctx.payload = round;
        xylem_run(_ts_main, &ctx, &_rt_opts);
        ASSERT(ctx.tested == 1);
    }
}

typedef struct {
    int tested;
} _bt_ctx_t;

typedef struct _test_channel_waiter_s _test_channel_waiter_t;

struct xylem_channel_s {
    mpsc_t                        queue;
    _Atomic(_test_channel_waiter_t*) waiter;
    _Atomic bool                  recv_in_progress;
    _Atomic int32_t               refcnt;
    size_t                        cap;
    _Atomic size_t                count;
    _Atomic uint32_t              send_gate;
};

typedef struct {
    mpsc_node_t node;
    void*       payload;
} _test_channel_msg_t;

typedef struct {
    xylem_channel_t*    ch;
    mpsc_node_t*        prev;
    _test_channel_msg_t* msg;
    int                 payload;
    bool                tail_claimed;
    int                 tested;
} _closed_transient_ctx_t;

typedef struct {
    xylem_channel_t*     ch;
    _test_channel_msg_t* msg;
    int                  payload;
    atomic_bool          close_returned;
    int                  tested;
} _close_wait_ctx_t;

static void _bt_coro(void* arg) {
    _bt_ctx_t* ctx = (_bt_ctx_t*)arg;
    xylem_channel_t* ch = xylem_channel_create(2);
    ASSERT(ch != NULL);
    ASSERT(xylem_channel_cap(ch) == 2);

    int a = 1, b = 2, c = 3;
    ASSERT(xylem_channel_send(ch, &a) == 0);
    ASSERT(xylem_channel_send(ch, &b) == 0);
    ASSERT(xylem_channel_len(ch) == 2);

    ASSERT(xylem_channel_send(ch, &c) == INT_MAX);

    ASSERT(xylem_channel_recv_timeout(ch, 0) == &a);
    ASSERT(xylem_channel_len(ch) == 1);
    ASSERT(xylem_channel_send(ch, &c) == 0);

    ASSERT(xylem_channel_recv_timeout(ch, 0) == &b);
    ASSERT(xylem_channel_recv_timeout(ch, 0) == &c);
    ASSERT(xylem_channel_recv_timeout(ch, 0) == NULL);

    ctx->tested = 1;
    xylem_channel_destroy(ch);
    xylem_shutdown();
}

static void _bt_main(void* arg) {
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    xylem_spawn(_bt_coro, arg);
}

static void test_bounded_try(void) {
    fprintf(stderr, "=== test_bounded_try\n");
    _bt_ctx_t ctx = {0};
    xylem_run(_bt_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

static void _closed_transient_linker(void* arg) {
    _closed_transient_ctx_t* ctx = (_closed_transient_ctx_t*)arg;
    xylem_sleep(1);
    if (!ctx->tail_claimed) {
        ctx->prev = atomic_exchange_explicit(
            &ctx->ch->queue.tail, &ctx->msg->node, memory_order_acq_rel);
    }
    atomic_store_explicit(
        &ctx->prev->next, &ctx->msg->node, memory_order_release);
}

static void _closed_transient_recv(void* arg) {
    _closed_transient_ctx_t* ctx = (_closed_transient_ctx_t*)arg;
    void* msg = xylem_channel_recv(ctx->ch);
    ASSERT(msg == &ctx->payload);
    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    xylem_shutdown();
}

static void _closed_transient_main(void* arg) {
    _closed_transient_ctx_t* ctx = (_closed_transient_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    ctx->ch = xylem_channel_create(0);
    ASSERT(ctx->ch != NULL);

    ctx->msg = (_test_channel_msg_t*)calloc(1, sizeof(_test_channel_msg_t));
    ASSERT(ctx->msg != NULL);
    ctx->msg->payload = &ctx->payload;
    atomic_store_explicit(&ctx->msg->node.next, NULL, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->ch->count, 1, memory_order_acq_rel);
    ctx->prev = atomic_exchange_explicit(
        &ctx->ch->queue.tail, &ctx->msg->node, memory_order_acq_rel);
    ctx->tail_claimed = true;
    atomic_store_explicit(
        &ctx->ch->send_gate,
        TEST_CHANNEL_SEND_GATE_CLOSED,
        memory_order_release);

    xylem_spawn(_closed_transient_recv, ctx);
    xylem_spawn(_closed_transient_linker, ctx);
}

static void test_closed_recv_waits_for_in_flight_send_link(void) {
    fprintf(stderr, "=== test_closed_recv_waits_for_in_flight_send_link\n");
    _closed_transient_ctx_t ctx = { .payload = 42 };
    xylem_opts_t opts = { .workers = 2 };
    xylem_run(_closed_transient_main, &ctx, &opts);
    ASSERT(ctx.tested == 1);
}

static void _closed_reserved_main(void* arg) {
    _closed_transient_ctx_t* ctx = (_closed_transient_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    ctx->ch = xylem_channel_create(0);
    ASSERT(ctx->ch != NULL);

    ctx->msg = (_test_channel_msg_t*)calloc(1, sizeof(_test_channel_msg_t));
    ASSERT(ctx->msg != NULL);
    ctx->msg->payload = &ctx->payload;
    atomic_store_explicit(&ctx->msg->node.next, NULL, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->ch->count, 1, memory_order_acq_rel);
    atomic_store_explicit(
        &ctx->ch->send_gate,
        TEST_CHANNEL_SEND_GATE_CLOSED,
        memory_order_release);

    xylem_spawn(_closed_transient_recv, ctx);
    xylem_spawn(_closed_transient_linker, ctx);
}

static void test_closed_recv_waits_for_reserved_send_before_push(void) {
    fprintf(stderr, "=== test_closed_recv_waits_for_reserved_send_before_push\n");
    _closed_transient_ctx_t ctx = { .payload = 43 };
    xylem_opts_t opts = { .workers = 2 };
    xylem_run(_closed_reserved_main, &ctx, &opts);
    ASSERT(ctx.tested == 1);
}

static void _close_wait_closer(void* arg) {
    _close_wait_ctx_t* ctx = (_close_wait_ctx_t*)arg;
    xylem_channel_close(ctx->ch);
    atomic_store_explicit(&ctx->close_returned, true, memory_order_release);
}

static void _close_wait_main(void* arg) {
    _close_wait_ctx_t* ctx = (_close_wait_ctx_t*)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    ctx->ch = xylem_channel_create(0);
    ASSERT(ctx->ch != NULL);

    atomic_fetch_add_explicit(
        &ctx->ch->send_gate, 1, memory_order_acq_rel);
    xylem_spawn(_close_wait_closer, ctx);

    while ((atomic_load_explicit(&ctx->ch->send_gate, memory_order_acquire)
            & TEST_CHANNEL_SEND_GATE_CLOSED) == 0) {
        xylem_sleep(1);
    }
    xylem_sleep(20);
    ASSERT(!atomic_load_explicit(
        &ctx->close_returned, memory_order_acquire));

    ctx->msg = (_test_channel_msg_t*)calloc(1, sizeof(_test_channel_msg_t));
    ASSERT(ctx->msg != NULL);
    ctx->msg->payload = &ctx->payload;
    mpsc_push(&ctx->ch->queue, &ctx->msg->node);
    atomic_fetch_add_explicit(&ctx->ch->count, 1, memory_order_acq_rel);
    atomic_fetch_sub_explicit(
        &ctx->ch->send_gate, 1, memory_order_acq_rel);

    while (!atomic_load_explicit(&ctx->close_returned, memory_order_acquire)) {
        xylem_sleep(1);
    }
    ASSERT(xylem_channel_recv_timeout(ctx->ch, 0) == &ctx->payload);

    ctx->tested = 1;
    xylem_channel_destroy(ctx->ch);
    xylem_shutdown();
}

static void test_close_waits_for_active_sender(void) {
    fprintf(stderr, "=== test_close_waits_for_active_sender\n");
    _close_wait_ctx_t ctx = { .payload = 44 };
    xylem_opts_t opts = { .workers = 2 };
    xylem_run(_close_wait_main, &ctx, &opts);
    ASSERT(ctx.tested == 1);
}

int main(void) {
    test_concurrent();
    test_timeout_empty();
    test_timeout_deliver();
    test_timeout_stale_timer_does_not_end_next_recv();
    test_timeout_race();
    test_thread_recv();
    test_thread_send();
    test_bounded_try();
    test_closed_recv_waits_for_in_flight_send_link();
    test_closed_recv_waits_for_reserved_send_before_push();
    test_close_waits_for_active_sender();
    return 0;
}
