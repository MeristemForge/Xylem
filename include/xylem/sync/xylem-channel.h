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

#include <limits.h>
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
 *     full bounded channel returns INT_MAX (drop/retry is
 *     the caller's choice -- there is no blocking backpressure). An
 *     unbounded channel never reports full.
 *   - recv() / recv_timeout(): callable from a coroutine OR an OS
 *     thread. Only one receiver may operate on a channel at a time;
 *     concurrent recv aborts (single-consumer MPSC contract).
 *   - create(), destroy() must be called from inside a coroutine
 *     (coroutine-only; they abort otherwise). close() is any-context.
 *
 * Capacity:
 *   - create(0) makes an unbounded channel: send never reports full
 *     (it always queues, barring OOM).
 *   - create(cap) with cap > 0 caps the in-flight message count at cap;
 *     send returns INT_MAX when full. For backpressure the
 *     receiver can watch len()/cap() and drop once over a threshold
 *     (recv_timeout(ch, 0) drains without blocking).
 */

/**
 * @brief Create a channel.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param cap  Maximum in-flight messages (sent but not yet received).
 *             0 makes the channel unbounded (send never reports full);
 *             a value > 0 caps the in-flight count, and send returns
 *             INT_MAX instead of queueing when full. send
 *             never blocks either way.
 *
 * @return Channel handle, or NULL on allocation failure.
 */
extern xylem_channel_t* xylem_channel_create(size_t cap);

/**
 * @brief Destroy the channel, releasing its memory.
 *
 * @note [COROUTINE-ONLY]
 *
 * Any messages still queued are freed (node wrapper only -- payload
 * lifetime is the caller's responsibility). Accepts NULL.
 *
 * @param ch  Channel handle.
 */
extern void xylem_channel_destroy(xylem_channel_t* ch);

/**
 * @brief Close the channel, signalling no more sends.
 *
 * @note [THREAD-SAFE]
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
 * Aborts if the channel is closed.
 *
 * @param ch   Channel handle.
 * @param msg  Opaque message pointer (must be non-NULL).
 *
 * @return 0 on success, INT_MAX if the channel is bounded
 *         and at capacity, or -1 on invalid input or allocation
 *         failure.
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
 * drop/backpressure decisions against cap().
 *
 * @param ch  Channel handle (NULL returns 0).
 *
 * @return Message count at the moment of the call.
 */
extern size_t xylem_channel_len(xylem_channel_t* ch);

/**
 * @brief Capacity of the channel.
 *
 * @note [THREAD-SAFE]
 *
 * @param ch  Channel handle (NULL returns 0).
 *
 * @return The cap passed to create(), or 0 for an unbounded channel.
 */
extern size_t xylem_channel_cap(xylem_channel_t* ch);
