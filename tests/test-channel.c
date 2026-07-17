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

#include "xylem/xylem-threads.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>


static xylem_opts_t _rt_opts = { .workers = 0 };

#define CH_SENDERS  20
#define CH_MESSAGES 10

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    atomic_int       recv_count;
    atomic_int       senders_done;
    atomic_int       tested;
} _ch_ctx_t;

static void _ch_sender(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    for (int i = 0; i < CH_MESSAGES; i++) {
        xylem_channel_send(ctx->ch, &ctx->payload);
    }
    atomic_fetch_add(&ctx->senders_done, 1);
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
    while (atomic_load(&ctx->senders_done) < CH_SENDERS) {
        xylem_sleep(1);
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _test_ch_main(void* arg) {
    _ch_ctx_t* ctx = (_ch_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
    xylem_spawn(_ch_receiver, ctx);
    for (int i = 0; i < CH_SENDERS; i++) {
        xylem_spawn(_ch_sender, ctx);
    }
}

static void test_concurrent(void) {
    fprintf(stderr, "=== test_concurrent\n");
    for (int round = 0; round < 20; round++) {
        _ch_ctx_t ctx = {0};
        _test_ch_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
    }
}

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    atomic_int       sender_done;
    atomic_int       tested;
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
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _to_basic_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
    xylem_spawn(_to_basic_coro, ctx);
}

static void test_timeout_empty(void) {
    fprintf(stderr, "=== test_timeout_empty\n");
    _to_ctx_t ctx = {0};
    _to_basic_main(&ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
    ASSERT(atomic_load(&ctx.tested) == 1);
}

static void _to_sender_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    xylem_sleep(20);
    xylem_channel_send(ctx->ch, &ctx->payload);
    atomic_store(&ctx->sender_done, 1);
}

static void _to_recv_coro(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    void* msg = xylem_channel_recv_timeout(ctx->ch, 1000);
    ASSERT(msg == &ctx->payload);
    while (atomic_load(&ctx->sender_done) == 0) {
        xylem_sleep(1);
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _to_deliver_main(void* arg) {
    _to_ctx_t* ctx = (_to_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
    xylem_spawn(_to_recv_coro, ctx);
    xylem_spawn(_to_sender_coro, ctx);
}

static void test_timeout_deliver(void) {
    fprintf(stderr, "=== test_timeout_deliver\n");
    _to_ctx_t ctx = {0};
    _to_deliver_main(&ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
    ASSERT(atomic_load(&ctx.tested) == 1);
}

#define STALE_TIMER_ROUNDS 100

typedef struct {
    atomic_int tested;
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
    xylem_channel_t* ch = xylem_channel_create();
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

    xylem_waitgroup_destroy(wg);
    xylem_channel_destroy(ch);
    atomic_store(&ctx->tested, 1);
}

static void _stale_main(void* arg) {
    xylem_spawn(_stale_recv_coro, arg);
}

static void test_timeout_stale_timer_does_not_end_next_recv(void) {
    fprintf(stderr, "=== test_timeout_stale_timer_does_not_end_next_recv\n");
    _stale_ctx_t ctx = {0};
    _stale_main(&ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
    ASSERT(atomic_load(&ctx.tested) == 1);
}

#define TO_RACE_ROUNDS 200

typedef struct {
    xylem_channel_t*   ch;
    xylem_waitgroup_t* wg;
    int                payload;
    uint64_t           timeout_ms;
    uint64_t           send_at_ms;
    atomic_int         got_timeout;
    atomic_int         got_message;
    atomic_int         tested;
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
        atomic_store(&ctx->got_message, 1);
    } else {
        atomic_store(&ctx->got_timeout, 1);
    }

    xylem_waitgroup_wait(ctx->wg);
    if (atomic_load(&ctx->got_timeout)) {
        void* leftover = xylem_channel_recv(ctx->ch);
        ASSERT(leftover == &ctx->payload);
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _race_main(void* arg) {
    _race_ctx_t* ctx = (_race_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
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
        _race_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }

        ASSERT(atomic_load(&ctx.got_message) ^
               atomic_load(&ctx.got_timeout));
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
    atomic_int         tested;
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
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    xylem_waitgroup_destroy(ctx->wg);
    ctx->wg = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _tr_main(void* arg) {
    _tr_ctx_t* ctx = (_tr_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
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
        _tr_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
    }
}

#define PRECOMMIT_SEND_ROUNDS  2000
#define PRECOMMIT_CLOSE_ROUNDS 500

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    thrd_t           thr;
    atomic_int       requested;
    atomic_int       completed;
    atomic_int       tested;
} _precommit_send_ctx_t;

static int _precommit_send_thread(void* arg) {
    _precommit_send_ctx_t* ctx = (_precommit_send_ctx_t*)arg;
    for (int round = 1; round <= PRECOMMIT_SEND_ROUNDS; round++) {
        while (atomic_load(&ctx->requested) < round) {
            thrd_yield();
        }
        ASSERT(xylem_channel_send(ctx->ch, &ctx->payload) == 0);
        atomic_store(&ctx->completed, round);
    }
    return 0;
}

static void _precommit_send_coro(void* arg) {
    _precommit_send_ctx_t* ctx = (_precommit_send_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
    ASSERT(ctx->ch != NULL);
    ASSERT(thrd_create(&ctx->thr, _precommit_send_thread, ctx)
           == thrd_success);

    for (int round = 1; round <= PRECOMMIT_SEND_ROUNDS; round++) {
        atomic_store(&ctx->requested, round);
        ASSERT(xylem_channel_recv(ctx->ch) == &ctx->payload);
    }

    thrd_join(ctx->thr, NULL);
    ASSERT(atomic_load(&ctx->completed) == PRECOMMIT_SEND_ROUNDS);
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    atomic_store(&ctx->tested, 1);
}

static void test_thread_send_precommit_race(void) {
    fprintf(stderr, "=== test_thread_send_precommit_race\n");
    _precommit_send_ctx_t ctx = {0};
    xylem_spawn(_precommit_send_coro, &ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
}

typedef struct {
    _Atomic(xylem_channel_t*) ch;
    thrd_t                    thr;
    atomic_int                requested;
    atomic_int                completed;
    atomic_int                tested;
} _precommit_close_ctx_t;

static int _precommit_close_thread(void* arg) {
    _precommit_close_ctx_t* ctx = (_precommit_close_ctx_t*)arg;
    for (int round = 1; round <= PRECOMMIT_CLOSE_ROUNDS; round++) {
        while (atomic_load(&ctx->requested) < round) {
            thrd_yield();
        }
        xylem_channel_t* ch = atomic_load(&ctx->ch);
        ASSERT(ch != NULL);
        xylem_channel_close(ch);
        atomic_store(&ctx->completed, round);
    }
    return 0;
}

static void _precommit_close_coro(void* arg) {
    _precommit_close_ctx_t* ctx = (_precommit_close_ctx_t*)arg;
    ASSERT(thrd_create(&ctx->thr, _precommit_close_thread, ctx)
           == thrd_success);

    for (int round = 1; round <= PRECOMMIT_CLOSE_ROUNDS; round++) {
        xylem_channel_t* ch = xylem_channel_create();
        ASSERT(ch != NULL);
        atomic_store(&ctx->ch, ch);
        atomic_store(&ctx->requested, round);
        ASSERT(xylem_channel_recv(ch) == NULL);
        while (atomic_load(&ctx->completed) < round) {
            thrd_yield();
        }
        xylem_channel_destroy(ch);
    }

    thrd_join(ctx->thr, NULL);
    atomic_store(&ctx->ch, NULL);
    atomic_store(&ctx->tested, 1);
}

static void test_thread_close_precommit_race(void) {
    fprintf(stderr, "=== test_thread_close_precommit_race\n");
    _precommit_close_ctx_t ctx = {0};
    xylem_spawn(_precommit_close_coro, &ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
}

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    thrd_t           thr;
    atomic_int       tested;
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
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    atomic_store(&ctx->tested, 1);
}

static void _ts_main(void* arg) {
    _ts_ctx_t* ctx = (_ts_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
    ASSERT(thrd_create(&ctx->thr, _ts_send_thread, ctx) == thrd_success);
    xylem_spawn(_ts_recv_coro, ctx);
}

static void test_thread_send(void) {
    fprintf(stderr, "=== test_thread_send\n");
    for (int round = 0; round < 20; round++) {
        _ts_ctx_t ctx = {0};
        ctx.payload = round;
        _ts_main(&ctx);
        while (atomic_load(&ctx.tested) == 0) {
            xylem_sleep(1);
        }
        ASSERT(atomic_load(&ctx.tested) == 1);
    }
}

typedef struct {
    xylem_channel_t* ch;
    int              payload;
    thrd_t           thr;
    atomic_bool      ready;
    atomic_bool      received;
    atomic_int       tested;
} _tc_ctx_t;

static int _tc_owner_thread(void* arg) {
    _tc_ctx_t* ctx = (_tc_ctx_t*)arg;
    ctx->ch = xylem_channel_create();
    ASSERT(ctx->ch != NULL);
    ASSERT(xylem_channel_send(ctx->ch, &ctx->payload) == 0);
    xylem_channel_close(ctx->ch);
    atomic_store(&ctx->ready, true);
    while (!atomic_load(&ctx->received)) {
        thrd_yield();
    }
    xylem_channel_destroy(ctx->ch);
    ctx->ch = NULL;
    return 0;
}

static void _tc_recv_coro(void* arg) {
    _tc_ctx_t* ctx = (_tc_ctx_t*)arg;
    while (!atomic_load(&ctx->ready)) {
        xylem_sleep(1);
    }
    void* msg = xylem_channel_recv(ctx->ch);
    ASSERT(msg == &ctx->payload);
    ASSERT(xylem_channel_recv(ctx->ch) == NULL);
    atomic_store(&ctx->received, true);
    thrd_join(ctx->thr, NULL);
    atomic_store(&ctx->tested, 1);
}

static void _tc_main(void* arg) {
    _tc_ctx_t* ctx = (_tc_ctx_t*)arg;
    ASSERT(thrd_create(&ctx->thr, _tc_owner_thread, ctx) == thrd_success);
    xylem_spawn(_tc_recv_coro, ctx);
}

static void test_thread_create_destroy(void) {
    fprintf(stderr, "=== test_thread_create_destroy\n");
    _tc_ctx_t ctx = {0};
    ctx.payload = 42;
    _tc_main(&ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
    ASSERT(atomic_load(&ctx.tested) == 1);
}

typedef struct {
    atomic_int tested;
} _bt_ctx_t;

static void _bt_coro(void* arg) {
    _bt_ctx_t* ctx = (_bt_ctx_t*)arg;
    xylem_channel_t* ch = xylem_channel_create();
    ASSERT(ch != NULL);

    int a = 1, b = 2, c = 3;
    ASSERT(xylem_channel_send(ch, &a) == 0);
    ASSERT(xylem_channel_send(ch, &b) == 0);
    ASSERT(xylem_channel_send(ch, &c) == 0);
    ASSERT(xylem_channel_len(ch) == 3);

    ASSERT(xylem_channel_recv_timeout(ch, 0) == &a);
    ASSERT(xylem_channel_len(ch) == 2);

    ASSERT(xylem_channel_recv_timeout(ch, 0) == &b);
    ASSERT(xylem_channel_recv_timeout(ch, 0) == &c);
    ASSERT(xylem_channel_recv_timeout(ch, 0) == NULL);

    xylem_channel_destroy(ch);
    atomic_store(&ctx->tested, 1);
}

static void _bt_main(void* arg) {
    xylem_spawn(_bt_coro, arg);
}

static void test_unbounded_len(void) {
    fprintf(stderr, "=== test_unbounded_len\n");
    _bt_ctx_t ctx = {0};
    _bt_main(&ctx);
    while (atomic_load(&ctx.tested) == 0) {
        xylem_sleep(1);
    }
    ASSERT(atomic_load(&ctx.tested) == 1);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_concurrent();
    test_timeout_empty();
    test_timeout_deliver();
    test_timeout_stale_timer_does_not_end_next_recv();
    test_timeout_race();
    test_thread_recv();
    test_thread_send_precommit_race();
    test_thread_close_precommit_race();
    test_thread_send();
    test_thread_create_destroy();
    test_unbounded_len();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, &_rt_opts);
    return 0;
}
