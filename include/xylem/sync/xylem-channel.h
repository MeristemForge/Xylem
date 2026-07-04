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

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_channel_s xylem_channel_t;

/**
 * MPSC channel: many senders, single receiver.
 *
 * Lock-free data path (intrusive MPSC queue) with a single-receiver
 * wakeup slot, so recv works from either a coroutine or a plain OS
 * thread: a coroutine producer can hand work to an OS-thread consumer
 * and vice versa.
 *
 * Threading:
 *   - send(): callable from any thread or coroutine; never parks. A
 *     channel never reports full; callers that need soft backpressure
 *     can watch len() and drop, yield, or retry above their own
 *     threshold. Like Go channels, send() must not race with close()
 *     on the same channel.
 *   - recv() / recv_timeout(): callable from a coroutine OR an OS
 *     thread. close() may race with recv() to wake the receiver. Only
 *     one receiver may operate on a channel at a time; concurrent recv
 *     aborts (single-consumer MPSC contract).
 *   - create(), destroy(), close() are any-context. create() requires
 *     the runtime to be running; destroy() must not race with other API
 *     calls on the same channel.
 *
 * Lifetime:
 *   - This object may wake coroutine waiters through the runtime
 *     scheduler. External OS threads must not call channel APIs after
 *     xylem_shutdown() has been called. Stop and join those threads
 *     before shutdown, or make sure they touch no channel once shutdown
 *     begins.
 *
 * Capacity:
 *   - Channels are unbounded. len() is a best-effort snapshot for
 *     observability and soft threshold policies; it is not a hard
 *     reservation and cannot enforce a bound under concurrent senders.
 */

/**
 * @brief Create a channel.
 *
 * @note [THREAD-SAFE]
 *
 * Must be called while the runtime is running.
 *
 * @return Channel handle, or NULL on allocation failure.
 */
extern xylem_channel_t* xylem_channel_create(void);

/**
 * @brief Destroy the channel, releasing its memory.
 *
 * @note [THREAD-SAFE]
 *
 * Any messages still queued are freed (node wrapper only -- payload
 * lifetime is the caller's responsibility). Accepts NULL. Must not race
 * with any other channel API on the same channel.
 *
 * @param ch  Channel handle.
 */
extern void xylem_channel_destroy(xylem_channel_t* ch);

/**
 * @brief Close the channel, signalling no more sends.
 *
 * @note [THREAD-SAFE]
 *
 * May race with recv() to wake the receiver. Like Go channels, close
 * must not race with send on the same channel: callers must stop all
 * producers before closing.
 *
 * After close:
 *   - recv() continues to return queued messages, then NULL.
 *   - send() aborts the process.
 *   - Closing again or closing NULL aborts.
 *
 * Does NOT free the channel; call destroy() after draining.
 *
 * @param ch  Channel handle.
 */
extern void xylem_channel_close(xylem_channel_t* ch);

/**
 * @brief Send a message. Non-blocking, thread-safe.
 *
 * @note [THREAD-SAFE]
 *
 * Aborts if the channel is closed. Must not race with close on the same
 * channel; callers must stop all producers before closing.
 *
 * @param ch   Channel handle.
 * @param msg  Opaque message pointer (must be non-NULL).
 *
 * @return 0 on success, or -1 on invalid input or allocation failure.
 */
extern int xylem_channel_send(xylem_channel_t* ch, void* msg);

/**
 * @brief Receive the next message, blocking the calling coroutine
 *        forever until a message arrives or the channel closes.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Equivalent to xylem_channel_recv_timeout(ch, (uint64_t)-1).
 * Concurrent recv from multiple coroutines aborts.
 *
 * @param ch  Channel handle.
 *
 * @return Message pointer, or NULL if the channel is closed and empty.
 */
extern void* xylem_channel_recv(xylem_channel_t* ch);

/**
 * @brief Receive the next message with a timeout.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Concurrent recv from multiple coroutines aborts (same single-
 * receiver contract as recv).
 *
 * @param ch          Channel handle.
 * @param timeout_ms  Wait policy, three cases:
 *                      - 0           : non-blocking try. Pops a queued
 *                                      message if one is immediately
 *                                      available, otherwise returns
 *                                      NULL at once without parking.
 *                      - (uint64_t)-1: block forever (identical to
 *                                      xylem_channel_recv).
 *                      - any other n : block up to n milliseconds.
 *
 * @return Message pointer, or NULL. NULL means, depending on the wait
 *         policy: nothing was immediately available (try), the timeout
 *         elapsed, or the channel is closed and empty. The NULL cases
 *         are not distinguished; callers needing the reason should
 *         track it out of band.
 */
extern void* xylem_channel_recv_timeout(
    xylem_channel_t* ch, uint64_t timeout_ms);

/**
 * @brief Current number of in-flight messages (sent but not received).
 *
 * @note [THREAD-SAFE]
 *
 * Best-effort snapshot, safe to call from any thread. Useful for
 * observability and soft drop/backoff thresholds. Not a reservation:
 * concurrent senders may exceed a threshold checked by len().
 *
 * @param ch  Channel handle (NULL returns 0).
 *
 * @return Message count at the moment of the call.
 */
extern size_t xylem_channel_len(xylem_channel_t* ch);
