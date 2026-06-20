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

_Pragma("once")

/* crypto */
#include "xylem/crypto/xylem-sha1.h"
#include "xylem/crypto/xylem-sha256.h"
#include "xylem/crypto/xylem-hmac256.h"
#include "xylem/crypto/xylem-aes256.h"

/* encoding */
#include "xylem/encoding/xylem-base64.h"
#include "xylem/encoding/xylem-bswap.h"
#include "xylem/encoding/xylem-varint.h"
#include "xylem/encoding/xylem-url.h"
#include "xylem/encoding/xylem-json.h"
#include "xylem/encoding/xylem-gzip.h"
#include "xylem/encoding/xylem-fec.h"

/* sync */
#include "xylem/sync/xylem-waitgroup.h"
#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"
#include "xylem/sync/xylem-cond.h"
#include "xylem/sync/xylem-sem.h"

/* container */
#include "xylem/container/xylem-ringbuf.h"
#include "xylem/container/xylem-list.h"
#include "xylem/container/xylem-stack.h"
#include "xylem/container/xylem-queue.h"
#include "xylem/container/xylem-heap.h"
#include "xylem/container/xylem-rbtree.h"

/* core */
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-serial.h"
#include "xylem/xylem-timer.h"
#include "xylem/xylem-ticker.h"
#include "xylem/xylem-threads.h"

/* net */
#include "xylem/net/xylem-tcp.h"
#include "xylem/net/xylem-udp.h"
#include "xylem/net/xylem-rudp.h"
#include "xylem/net/xylem-uds.h"
#include "xylem/net/xylem-mux.h"
#include "xylem/net/xylem-reader.h"
#include "xylem/net/xylem-writer.h"
#include "xylem/net/xylem-tls.h"
#include "xylem/net/xylem-dtls.h"
#include "xylem/net/http/xylem-http.h"
#include "xylem/net/http/xylem-http-auth.h"
#include "xylem/net/http/xylem-http-cookie.h"
#include "xylem/net/http/xylem-http-form.h"
#include "xylem/net/http/xylem-http-multipart.h"
#include "xylem/net/http/xylem-http-router.h"
#include "xylem/net/http/xylem-http-cors.h"
#include "xylem/net/http/xylem-http-fileserver.h"
#include "xylem/net/xylem-ws.h"

/* runtime */
#include <stdint.h>

typedef struct xylem_opts_s {
    int32_t workers;  /* Scheduler worker count, 0 for default (CPU count). */
    size_t  coro_stack_size; /* Coroutine stack size in bytes, 0 for default. */
} xylem_opts_t;

/**
 * @brief Run the xylem runtime until all coroutines exit.
 *
 * Blocks the calling thread until every spawned coroutine has
 * returned or xylem_shutdown() is called. Boots the scheduler and
 * blocking-task pool under the hood, spawns @p main_fn as the root
 * coroutine, and tears the runtime down on return.
 *
 * @param main_fn  Initial coroutine entry point.
 * @param arg      Opaque argument passed to main_fn.
 * @param opts     Runtime options, NULL for defaults.
 */
extern void xylem_run(
    void (*main_fn)(void*), void* arg, xylem_opts_t* opts);

/**
 * @brief Signal the runtime to shut down.
 *
 * @note [THREAD-SAFE]
 *
 * Unblocks xylem_run() without waiting for coroutines to finish
 * naturally. Already-running blocking jobs submitted by xylem_await()
 * are not killed; they must return before the blocking pool can finish
 * teardown.
 *
 * After shutdown is requested, external OS threads must not keep using
 * runtime-backed objects such as channels, mutexes, conds, semaphores,
 * waitgroups, timers, or tickers. Stop and join those threads before
 * calling xylem_shutdown(), or make sure they return without touching
 * Xylem objects once shutdown begins.
 */
extern void xylem_shutdown(void);

/**
 * @brief Spawn a new coroutine on the runtime.
 *
 * @note [THREAD-SAFE]
 *
 * @param fn   Coroutine entry function.
 * @param arg  Opaque argument.
 */
extern void xylem_spawn(void (*fn)(void*), void* arg);

/**
 * @brief Suspend the caller for @p ms milliseconds.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * On a coroutine running on the runtime, this parks the coroutine (the
 * worker thread stays free for other work). On a plain OS thread, it
 * blocks that thread for the duration. Either context is valid.
 */
extern void xylem_sleep(uint64_t ms);

/**
 * @brief Await a blocking function run on the runtime's thread pool.
 *
 * @note [COROUTINE-ONLY]
 *
 * Hands @p fn to the blocking pool and suspends the calling coroutine
 * until it returns; the coroutine then resumes on a scheduler worker.
 * This keeps the worker free to run other coroutines while @p fn
 * blocks. Must be called from a coroutine on the runtime -- calling it
 * off a coroutine aborts.
 *
 * xylem_shutdown() does not cancel @p fn once it has started running on
 * a blocking-pool thread. The function must eventually return, or
 * runtime teardown will wait for that thread.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_await(void (*fn)(void*), void* arg);
