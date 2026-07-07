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
#include "xylem/xylem-utils.h"

#include "container/mpsc.h"
#include "platform/platform-cpu.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "sync/thrd-wake.h"

#include "runtime/minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct _msg_s {
    mpsc_node_t node;
    void*       payload;
} _msg_t;

typedef enum _waiter_kind_e {
    WAITER_CORO,
    WAITER_THRD,
} _waiter_kind_t;

typedef struct _waiter_s {
    _waiter_kind_t   kind;
    xylem_channel_t* ch;
} _waiter_t;

typedef struct _coro_waiter_s {
    _waiter_t          base;
    mco_coro*          co;
    uint64_t           timeout_ms;
    scheduler_timer_t* timer;
    _Atomic int32_t    refcnt;
    _Atomic bool       timer_fired;
} _coro_waiter_t;

typedef struct _thrd_waiter_s {
    _waiter_t    base;
    thrd_wake_t* wake;
} _thrd_waiter_t;

struct xylem_channel_s {
    mpsc_t              queue;
    _Atomic(_waiter_t*) waiter;
    _Atomic bool        closed;
    _Atomic bool        receiving;
    _Atomic int32_t     refcnt;
    scheduler_t*        sched;
    _Atomic size_t      count;
};

static void _channel_wake(xylem_channel_t* ch, _waiter_t* w) {
    if (w->kind == WAITER_CORO) {
        _coro_waiter_t* cw = (_coro_waiter_t*)w;
        scheduler_schedule(ch->sched, cw->co);
    } else {
        _thrd_waiter_t* tw = (_thrd_waiter_t*)w;
        thrd_wake_signal(tw->wake);
    }
}

static inline void _channel_ref(xylem_channel_t* ch) {
    atomic_fetch_add(&ch->refcnt, 1);
}

static void _channel_unref(xylem_channel_t* ch) {
    if (atomic_fetch_sub(&ch->refcnt, 1) != 1) {
        return;
    }
    mpsc_node_t* node;
    while ((node = mpsc_pop(&ch->queue)) != NULL) {
        _msg_t* msg = mpsc_entry(node, _msg_t, node);
        free(msg);
    }
    free(ch);
}

static bool _channel_park_cb(mco_coro* co, void* arg) {
    _coro_waiter_t*  w  = (_coro_waiter_t*)arg;
    xylem_channel_t* ch = w->base.ch;

    w->co = co;
    atomic_store(&ch->waiter, &w->base);

    /* Avoid sleeping after a send/close/timer wins the race to wake us. */
    if (atomic_load(&ch->closed)
        || (w->timer && atomic_load(&w->timer_fired))
        || mpsc_can_pop(&ch->queue)) {
        _waiter_t* expected = &w->base;
        if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
            return false;
        }
    }
    return true;
}

static void _channel_coro_timed_ref(_coro_waiter_t* w) {
    atomic_fetch_add(&w->refcnt, 1);
}

static void _channel_coro_timed_unref(_coro_waiter_t* w) {
    if (atomic_fetch_sub(&w->refcnt, 1) != 1) {
        return;
    }
    if (w->timer) {
        scheduler_timer_destroy(w->timer);
    }
    free(w);
}

static void _channel_timeout_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    _coro_waiter_t*  w  = (_coro_waiter_t*)ud;
    xylem_channel_t* ch = w->base.ch;
    atomic_store(&w->timer_fired, true);

    _waiter_t* expected = &w->base;
    if (atomic_compare_exchange_strong(&ch->waiter, &expected, NULL)) {
        _channel_wake(ch, &w->base);
    }

    _channel_coro_timed_unref(w);
    _channel_unref(ch);
}

static void _channel_timedwait_cleanup_cb(mco_coro* co, void* arg) {
    (void)co;
    _coro_waiter_t*  w  = (_coro_waiter_t*)arg;
    xylem_channel_t* ch = w->base.ch;
    if (scheduler_timer_stop(w->timer)) {
        _channel_coro_timed_unref(w);
        _channel_unref(ch);
    }
    _channel_coro_timed_unref(w);
}

static void* _channel_try_take(xylem_channel_t* ch) {
    mpsc_node_t* node = mpsc_pop(&ch->queue);
    if (!node) {
        return NULL;
    }

    _msg_t* m       = mpsc_entry(node, _msg_t, node);
    void*   payload = m->payload;
    free(m);
    atomic_fetch_sub(&ch->count, 1);
    return payload;
}

static bool _channel_wait_pending_send(xylem_channel_t* ch) {
    if (atomic_load(&ch->count) == 0) {
        return false;
    }
    if (mco_running()) {
        runtime_yield_credit();
    } else {
        platform_cpu_relax();
    }
    return true;
}

static bool _channel_cancel_waiter(xylem_channel_t* ch, _waiter_t* w) {
    _waiter_t* expected = w;
    return atomic_compare_exchange_strong(&ch->waiter, &expected, NULL);
}

static bool _channel_publish_waiter(xylem_channel_t* ch, _waiter_t* w) {
    atomic_store(&ch->waiter, w);

    /* Avoid lost wakeups between the failed pop and waiter publish. */
    if (atomic_load(&ch->closed) || mpsc_can_pop(&ch->queue)) {
        return !_channel_cancel_waiter(ch, w);
    } else {
        return true;
    }
}

static void* _channel_wait_coro(xylem_channel_t* ch) {
    _coro_waiter_t w;
    w.base.kind = WAITER_CORO;
    w.base.ch   = ch;
    w.co        = NULL;
    w.timer     = NULL;

    void* payload = NULL;
    for (;;) {
        payload = _channel_try_take(ch);
        if (payload) {
            break;
        }
        if (atomic_load(&ch->closed)) {
            if (!_channel_wait_pending_send(ch)) {
                break;
            }
            continue;
        }

        scheduler_park(ch->sched, _channel_park_cb, NULL, &w);
    }
    return payload;
}

static void* _channel_timedwait_coro(
        xylem_channel_t* ch,
        uint64_t timeout_ms) {
    _coro_waiter_t* w = (_coro_waiter_t*)calloc(1, sizeof(_coro_waiter_t));
    if (!w) {
        return _channel_try_take(ch);
    }
    w->timer = scheduler_timer_create(ch->sched);
    if (!w->timer) {
        free(w);
        return _channel_try_take(ch);
    }
    w->base.kind  = WAITER_CORO;
    w->base.ch    = ch;
    w->co         = NULL;
    w->timeout_ms = timeout_ms;
    atomic_init(&w->refcnt, 1);
    atomic_init(&w->timer_fired, false);

    _channel_ref(ch);
    _channel_coro_timed_ref(w);
    scheduler_timer_start(
            w->timer,
            _channel_timeout_cb,
            w,
            w->timeout_ms,
            0);

    void* payload = NULL;
    for (;;) {
        payload = _channel_try_take(ch);
        if (payload) {
            break;
        }
        if (atomic_load(&w->timer_fired)) {
            break;
        }
        if (atomic_load(&ch->closed)) {
            if (!_channel_wait_pending_send(ch)) {
                break;
            }
            continue;
        }

        scheduler_park(ch->sched,
                       _channel_park_cb,
                       _channel_timedwait_cleanup_cb,
                       w);
    }

    if (scheduler_timer_stop(w->timer)) {
        _channel_coro_timed_unref(w);
        _channel_unref(ch);
    }
    _channel_coro_timed_unref(w);
    return payload;
}

static void* _channel_wait_thrd(xylem_channel_t* ch) {
    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.ch   = ch;
    w.wake      = thrd_wake_self();

    void* payload = NULL;
    for (;;) {
        payload = _channel_try_take(ch);
        if (payload) {
            break;
        }
        if (atomic_load(&ch->closed)) {
            if (!_channel_wait_pending_send(ch)) {
                break;
            }
            continue;
        }

        if (!_channel_publish_waiter(ch, &w.base)) {
            continue;
        }

        thrd_wake_wait(w.wake);
    }
    return payload;
}

static void* _channel_timedwait_thrd(
        xylem_channel_t* ch,
        uint64_t timeout_ms) {
    _thrd_waiter_t w;
    w.base.kind = WAITER_THRD;
    w.base.ch   = ch;
    w.wake      = thrd_wake_self();

    uint64_t deadline_ms =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    void* payload = NULL;
    for (;;) {
        payload = _channel_try_take(ch);
        if (payload) {
            break;
        }
        if (atomic_load(&ch->closed)) {
            if (!_channel_wait_pending_send(ch)) {
                break;
            }
            continue;
        }

        if (!_channel_publish_waiter(ch, &w.base)) {
            continue;
        }

        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now < deadline_ms) {
            uint64_t remaining = deadline_ms - now;
            if (thrd_wake_timedwait(w.wake, remaining)) {
                continue;
            }
        }

        /* CAS failure means a wake is already in flight. */
        if (_channel_cancel_waiter(ch, &w.base)) {
            break;
        }
        thrd_wake_wait(w.wake);
    }
    return payload;
}

static bool _channel_recv_enter(xylem_channel_t* ch) {
    if (!ch) {
        xylem_loge("<channel> recv on NULL channel");
        return false;
    }

    if (atomic_exchange(&ch->receiving, true)) {
        xylem_loge("<channel> concurrent recv violates single-receiver "
                   "contract ch=%p; aborting",
                   (void*)ch);
        abort();
    }
    return true;
}

static void* _channel_recv_leave(xylem_channel_t* ch, void* payload) {
    atomic_store(&ch->receiving, false);
    if (payload) {
        if (runtime_consume_credit(RUNTIME_CREDIT_COST)) {
            runtime_yield_credit();
        }
    }
    return payload;
}

static void* _channel_recv_impl(xylem_channel_t* ch) {
    if (!_channel_recv_enter(ch)) {
        return NULL;
    }

    void* payload;
    if (mco_running()) {
        payload = _channel_wait_coro(ch);
    } else {
        payload = _channel_wait_thrd(ch);
    }
    return _channel_recv_leave(ch, payload);
}

static void* _channel_recv_timeout_impl(
        xylem_channel_t* ch,
        uint64_t timeout_ms) {
    if (!_channel_recv_enter(ch)) {
        return NULL;
    }

    void* payload;
    if (timeout_ms == 0) {
        payload = _channel_try_take(ch);
    } else if (mco_running()) {
        payload = _channel_timedwait_coro(ch, timeout_ms);
    } else {
        payload = _channel_timedwait_thrd(ch, timeout_ms);
    }
    return _channel_recv_leave(ch, payload);
}

xylem_channel_t* xylem_channel_create(void) {
    scheduler_t* sched = runtime_get_scheduler();
    if (!sched) {
        xylem_loge("<channel> create without running runtime");
        return NULL;
    }

    xylem_channel_t* ch =
        (xylem_channel_t*)calloc(1, sizeof(xylem_channel_t));
    if (!ch) {
        return NULL;
    }
    mpsc_init(&ch->queue);
    atomic_init(&ch->waiter, NULL);
    atomic_init(&ch->closed, false);
    atomic_init(&ch->receiving, false);
    atomic_init(&ch->refcnt, 1);
    ch->sched = sched;
    atomic_init(&ch->count, 0);
    return ch;
}

void xylem_channel_close(xylem_channel_t* ch) {
    if (!ch) {
        xylem_loge("<channel> close on NULL channel; aborting");
        abort();
    }
    if (atomic_exchange(&ch->closed, true)) {
        xylem_loge("<channel> double close ch=%p; aborting", (void*)ch);
        abort();
    }
    _waiter_t* w = atomic_exchange(&ch->waiter, NULL);
    if (w) {
        _channel_wake(ch, w);
    }
}

void xylem_channel_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }

    _channel_unref(ch);
}

int xylem_channel_send(xylem_channel_t* ch, void* msg) {
    if (!ch || !msg) {
        xylem_loge("<channel> send NULL argument ch=%p msg=%p",
                   (void*)ch, msg);
        return -1;
    }

    if (atomic_load(&ch->closed)) {
        xylem_loge("<channel> send on closed channel ch=%p; aborting",
                   (void*)ch);
        abort();
    }

    _msg_t* m = (_msg_t*)calloc(1, sizeof(_msg_t));
    if (!m) {
        return -1;
    }

    m->payload = msg;
    atomic_fetch_add(&ch->count, 1);
    mpsc_push(&ch->queue, &m->node);

    _waiter_t* w = atomic_exchange(&ch->waiter, NULL);
    if (w) {
        _channel_wake(ch, w);
    }

    if (runtime_consume_credit(RUNTIME_CREDIT_COST)) {
        runtime_yield_credit();
    }
    return 0;
}

void* xylem_channel_recv(xylem_channel_t* ch) {
    return _channel_recv_impl(ch);
}

void* xylem_channel_recv_timeout(
        xylem_channel_t* ch, uint64_t timeout_ms) {
    if (timeout_ms == (uint64_t)-1) {
        return _channel_recv_impl(ch);
    }
    return _channel_recv_timeout_impl(ch, timeout_ms);
}

size_t xylem_channel_len(xylem_channel_t* ch) {
    if (!ch) {
        return 0;
    }
    return atomic_load(&ch->count);
}
