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
#include "xylem/xylem-threads.h"
#include "assert.h"
#include "utils.h"

#include "platform/platform-socket.h"
#include "runtime/iowait.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define READINESS_ROUNDS      500
#define TIMEOUT_ROUNDS        100
#define REUSE_ROUNDS          200
#define DEADLINE_RACE_THREADS 4
#define DEADLINE_RACE_ITERS   2000

typedef struct {
    platform_sock_t socks[2];
    iowait_t*       active;
} _socket_ctx_t;

typedef struct {
    iowait_t*       active;
    xylem_sem_t*    started;
    xylem_sem_t*    done;
    iowait_result_t result;
    _Atomic bool   finished;
    bool           write;
} _wait_ctx_t;

typedef struct {
    platform_sock_t sock;
    xylem_sem_t*     next;
} _sender_ctx_t;

typedef struct {
    platform_sock_t sock;
    size_t          remaining;
} _peer_ctx_t;

typedef struct {
    _socket_ctx_t* owner;
    xylem_sem_t*   done;
    _Atomic bool  finished;
} _reader_ctx_t;

typedef struct {
    iowait_t*    active;
    _Atomic bool start;
    _Atomic bool close_started;
    atomic_int   entered;
} _deadline_ctx_t;

static void _socket_open(_socket_ctx_t* ctx) {
    ASSERT(platform_socket_socketpair(AF_INET, SOCK_STREAM, 0, ctx->socks)
           == 0);
    platform_socket_enable_nonblocking(ctx->socks[0], true);
    platform_socket_enable_nonblocking(ctx->socks[1], true);
    ctx->active = iowait_create(ctx->socks[0]);
    ASSERT(ctx->active != NULL);
}

static void _socket_dispose(_socket_ctx_t* ctx) {
    iowait_destroy(ctx->active);
    platform_socket_close(ctx->socks[0]);
    platform_socket_close(ctx->socks[1]);
}

static bool _socket_would_block(void) {
    int err = platform_socket_get_lasterror();
    return err == PLATFORM_SO_ERROR_EAGAIN
           || err == PLATFORM_SO_ERROR_EWOULDBLOCK;
}

static void _socket_receive_byte(_socket_ctx_t* ctx, uint8_t expected) {
    uint8_t byte = 0;
    for (;;) {
        ssize_t n = platform_socket_recv(ctx->socks[0], &byte, 1);
        if (n == 1) {
            ASSERT(byte == expected);
            return;
        }
        ASSERT(n == -1 && _socket_would_block());
        ASSERT(iowait_read(ctx->active) == IOWAIT_READY);
    }
}

static void _wait_finished(_Atomic bool* finished) {
    while (!atomic_load(finished)) {
        xylem_sleep(0);
    }
}

static void _wait_coro(void* arg) {
    _wait_ctx_t* ctx = (_wait_ctx_t*)arg;
    xylem_sem_post(ctx->started);
    do {
        ctx->result = ctx->write ? iowait_write(ctx->active)
                                 : iowait_read(ctx->active);
    } while (ctx->result == IOWAIT_READY);
    xylem_sem_post(ctx->done);
    atomic_store(&ctx->finished, true);
}

static int _sender_thread(void* arg) {
    _sender_ctx_t* ctx = (_sender_ctx_t*)arg;
    for (int i = 0; i < READINESS_ROUNDS; i++) {
        uint8_t byte = (uint8_t)i;
        xylem_sem_wait(ctx->next);
        ASSERT(platform_socket_send(ctx->sock, &byte, 1) == 1);
    }
    return 0;
}

static int _peer_thread(void* arg) {
    _peer_ctx_t* ctx = (_peer_ctx_t*)arg;
    uint8_t buf[4096];
    while (ctx->remaining > 0) {
        int size = ctx->remaining < sizeof(buf)
                       ? (int)ctx->remaining : (int)sizeof(buf);
        ssize_t n = platform_socket_recv(ctx->sock, buf, size);
        ASSERT(n > 0);
        ctx->remaining -= (size_t)n;
        if (ctx->remaining == 0) {
            ASSERT(buf[n - 1] == 0x5a);
        }
    }
    uint8_t reply = 0xa5;
    ASSERT(platform_socket_send(ctx->sock, &reply, 1) == 1);
    return 0;
}

static void _reader_coro(void* arg) {
    _reader_ctx_t* ctx = (_reader_ctx_t*)arg;
    _socket_receive_byte(ctx->owner, 0xa5);
    xylem_sem_post(ctx->done);
    atomic_store(&ctx->finished, true);
}

static int _deadline_setter_thread(void* arg) {
    _deadline_ctx_t* ctx = (_deadline_ctx_t*)arg;
    while (!atomic_load(&ctx->start)) {
        thrd_yield();
    }
    atomic_fetch_add(&ctx->entered, 1);
    int i = 0;
    do {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        iowait_set_rd_deadline(ctx->active, now + 1000 + (uint64_t)(i & 7));
        iowait_set_wr_deadline(ctx->active, now + 1000 + (uint64_t)(i & 7));
        if ((i & 3) == 0) {
            iowait_set_rd_deadline(ctx->active, 0);
            iowait_set_wr_deadline(ctx->active, 0);
        }
        i++;
    } while (i < DEADLINE_RACE_ITERS || !atomic_load(&ctx->close_started));
    return 0;
}

static void test_readiness_before_wait(void) {
    _socket_ctx_t ctx = {0};
    uint8_t byte = 0x5a;
    _socket_open(&ctx);
    ASSERT(platform_socket_send(ctx.socks[1], &byte, 1) == 1);
    ASSERT(iowait_read(ctx.active) == IOWAIT_READY);
    _socket_receive_byte(&ctx, byte);
    _socket_dispose(&ctx);
}

static void test_thread_readiness_races_wait(void) {
    _socket_ctx_t owner = {0};
    _socket_open(&owner);
    _sender_ctx_t ctx = {
        .sock = owner.socks[1],
        .next = xylem_sem_create(0),
    };
    ASSERT(ctx.next != NULL);
    thrd_t thread;
    ASSERT(thrd_create(&thread, _sender_thread, &ctx) == thrd_success);
    for (int i = 0; i < READINESS_ROUNDS; i++) {
        xylem_sem_post(ctx.next);
        _socket_receive_byte(&owner, (uint8_t)i);
    }
    ASSERT(thrd_join(thread, NULL) == thrd_success);
    xylem_sem_destroy(ctx.next);
    _socket_dispose(&owner);
}

static void test_read_deadline_expires(void) {
    _socket_ctx_t ctx = {0};
    _socket_open(&ctx);
    iowait_set_rd_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 10);
    ASSERT(iowait_read(ctx.active) == IOWAIT_TIMEOUT);
    ASSERT(iowait_read_deadline_expired(ctx.active));
    _socket_dispose(&ctx);
}

static void test_deadline_clear_allows_read(void) {
    _socket_ctx_t ctx = {0};
    _socket_open(&ctx);
    iowait_set_rd_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    ASSERT(iowait_read(ctx.active) == IOWAIT_TIMEOUT);
    iowait_set_rd_deadline(ctx.active, 0);
    ASSERT(!iowait_read_deadline_expired(ctx.active));
    uint8_t byte = 0x5a;
    ASSERT(platform_socket_send(ctx.socks[1], &byte, 1) == 1);
    ASSERT(iowait_read(ctx.active) == IOWAIT_READY);
    _socket_receive_byte(&ctx, byte);
    _socket_dispose(&ctx);
}

static void test_write_deadline_expires(void) {
    _socket_ctx_t ctx = {0};
    _socket_open(&ctx);
    iowait_set_wr_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    iowait_result_t result;
    do {
        result = iowait_write(ctx.active);
    } while (result == IOWAIT_READY);
    ASSERT(result == IOWAIT_TIMEOUT);
    ASSERT(iowait_write_deadline_expired(ctx.active));
    iowait_set_wr_deadline(ctx.active, 0);
    ASSERT(!iowait_write_deadline_expired(ctx.active));
    ASSERT(iowait_write(ctx.active) == IOWAIT_READY);
    _socket_dispose(&ctx);
}

static void test_deadlines_isolate_directions(void) {
    _socket_ctx_t ctx = {0};
    _socket_open(&ctx);
    iowait_set_rd_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    ASSERT(iowait_read_deadline_expired(ctx.active));
    ASSERT(!iowait_write_deadline_expired(ctx.active));
    ASSERT(iowait_write(ctx.active) == IOWAIT_READY);

    iowait_set_rd_deadline(ctx.active, 0);
    iowait_set_wr_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    ASSERT(!iowait_read_deadline_expired(ctx.active));
    ASSERT(iowait_write_deadline_expired(ctx.active));
    uint8_t byte = 0x5a;
    ASSERT(platform_socket_send(ctx.socks[1], &byte, 1) == 1);
    ASSERT(iowait_read(ctx.active) == IOWAIT_READY);
    _socket_receive_byte(&ctx, byte);
    iowait_set_wr_deadline(ctx.active, 0);
    ASSERT(!iowait_write_deadline_expired(ctx.active));
    _socket_dispose(&ctx);
}

static void test_close_before_wait(void) {
    _socket_ctx_t ctx = {0};
    _socket_open(&ctx);
    iowait_set_rd_deadline(
        ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    iowait_close(ctx.active);
    iowait_close(ctx.active);
    ASSERT(iowait_read(ctx.active) == IOWAIT_CLOSED);
    iowait_result_t result = iowait_write(ctx.active);
    if (result == IOWAIT_READY) {
        result = iowait_write(ctx.active);
    }
    ASSERT(result == IOWAIT_CLOSED);
    _socket_dispose(&ctx);
}

static void test_close_races_both_waiters(void) {
    for (int i = 0; i < TIMEOUT_ROUNDS; i++) {
        _socket_ctx_t owner = {0};
        _socket_open(&owner);
        xylem_sem_t* started = xylem_sem_create(0);
        xylem_sem_t* done = xylem_sem_create(0);
        ASSERT(started != NULL && done != NULL);
        _wait_ctx_t rd = {
            .active = owner.active, .started = started, .done = done,
        };
        _wait_ctx_t wr = {
            .active = owner.active, .started = started, .done = done,
            .write = true,
        };
        xylem_spawn(_wait_coro, &rd);
        xylem_spawn(_wait_coro, &wr);
        xylem_sem_wait(started);
        xylem_sem_wait(started);
        iowait_close(owner.active);
        xylem_sem_wait(done);
        xylem_sem_wait(done);
        _wait_finished(&rd.finished);
        _wait_finished(&wr.finished);
        ASSERT(rd.result == IOWAIT_CLOSED);
        ASSERT(wr.result == IOWAIT_CLOSED);
        xylem_sem_destroy(started);
        xylem_sem_destroy(done);
        _socket_dispose(&owner);
    }
}

static void test_deadline_update_races_wait(void) {
    _socket_ctx_t owner = {0};
    _socket_open(&owner);
    xylem_sem_t* started = xylem_sem_create(0);
    xylem_sem_t* done = xylem_sem_create(0);
    ASSERT(started != NULL && done != NULL);
    for (int i = 0; i < TIMEOUT_ROUNDS; i++) {
        _wait_ctx_t ctx = {
            .active = owner.active, .started = started, .done = done,
        };
        xylem_spawn(_wait_coro, &ctx);
        xylem_sem_wait(started);
        iowait_set_rd_deadline(
            owner.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
        xylem_sem_wait(done);
        _wait_finished(&ctx.finished);
        ASSERT(ctx.result == IOWAIT_TIMEOUT);
        iowait_set_rd_deadline(owner.active, 0);
    }
    xylem_sem_destroy(started);
    xylem_sem_destroy(done);
    _socket_dispose(&owner);
}

static void test_read_and_write_progress_independently(void) {
    _socket_ctx_t owner = {0};
    _socket_open(&owner);
    platform_socket_set_sndbuf(owner.socks[0], 4096);
    uint8_t fill[4096] = {0};
    size_t sent = 0;
    for (;;) {
        ssize_t n = platform_socket_send(owner.socks[0], fill, sizeof(fill));
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        ASSERT(n == -1 && _socket_would_block());
        break;
    }
    ASSERT(sent > 0);
    platform_socket_enable_nonblocking(owner.socks[1], false);
    _peer_ctx_t peer = {.sock = owner.socks[1], .remaining = sent + 1};
    xylem_sem_t* done = xylem_sem_create(0);
    ASSERT(done != NULL);
    _reader_ctx_t reader = {.owner = &owner, .done = done};
    xylem_spawn(_reader_coro, &reader);
    thrd_t thread;
    ASSERT(thrd_create(&thread, _peer_thread, &peer) == thrd_success);
    uint8_t byte = 0x5a;
    for (;;) {
        ASSERT(iowait_write(owner.active) == IOWAIT_READY);
        ssize_t n = platform_socket_send(owner.socks[0], &byte, 1);
        if (n == 1) {
            break;
        }
        ASSERT(n == -1 && _socket_would_block());
    }
    xylem_sem_wait(done);
    _wait_finished(&reader.finished);
    ASSERT(thrd_join(thread, NULL) == thrd_success);
    ASSERT(peer.remaining == 0);
    xylem_sem_destroy(done);
    _socket_dispose(&owner);
}

static void test_handle_reuse_with_real_readiness(void) {
    _socket_ctx_t ctx = {0};
    _socket_open(&ctx);
    for (int i = 0; i < REUSE_ROUNDS; i++) {
        iowait_destroy(ctx.active);
        ctx.active = iowait_create(ctx.socks[0]);
        ASSERT(ctx.active != NULL);
        iowait_set_rd_deadline(
            ctx.active, xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
        ASSERT(iowait_read(ctx.active) == IOWAIT_TIMEOUT);
        iowait_set_rd_deadline(ctx.active, 0);
        uint8_t byte = (uint8_t)i;
        ASSERT(platform_socket_send(ctx.socks[1], &byte, 1) == 1);
        ASSERT(iowait_read(ctx.active) == IOWAIT_READY);
        _socket_receive_byte(&ctx, byte);
    }
    _socket_dispose(&ctx);
}

static void test_concurrent_deadline_setters_and_close(void) {
    _socket_ctx_t owner = {0};
    _socket_open(&owner);
    _deadline_ctx_t ctx = {.active = owner.active};
    thrd_t threads[DEADLINE_RACE_THREADS];
    for (int i = 0; i < DEADLINE_RACE_THREADS; i++) {
        ASSERT(thrd_create(&threads[i], _deadline_setter_thread, &ctx)
               == thrd_success);
    }
    atomic_store(&ctx.start, true);
    while (atomic_load(&ctx.entered) < DEADLINE_RACE_THREADS) {
        thrd_yield();
    }
    iowait_close(owner.active);
    atomic_store(&ctx.close_started, true);
    for (int i = 0; i < DEADLINE_RACE_THREADS; i++) {
        ASSERT(thrd_join(threads[i], NULL) == thrd_success);
    }
    ASSERT(iowait_read(owner.active) == IOWAIT_CLOSED);
    _socket_dispose(&owner);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);
    test_readiness_before_wait();
    test_thread_readiness_races_wait();
    test_read_deadline_expires();
    test_deadline_clear_allows_read();
    test_write_deadline_expires();
    test_deadlines_isolate_directions();
    test_close_before_wait();
    test_close_races_both_waiters();
    test_deadline_update_races_wait();
    test_read_and_write_progress_independently();
    test_handle_reuse_with_real_readiness();
    test_concurrent_deadline_setters_and_close();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}
