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

typedef struct xylem_channel_s xylem_channel_t;

/**
 * Channel concurrency model
 *
 * A xylem_channel is MPSC: many senders, a single receiver. Shape
 * matches tokio's mpsc / Rust's std::sync::mpsc; differs from Go's
 * chan, which permits many receivers.
 *
 * Threading:
 *   - send(), destroy() are safe from any thread.
 *   - recv() must be called from inside a coroutine on a scheduler
 *     worker (it parks). Only one coroutine may be the receiver for
 *     the lifetime of the channel.
 *
 * Misuse that aborts the process:
 *   - Two coroutines calling recv() on the same channel
 *     concurrently. Detected when the second recv tries to publish
 *     its park slot.
 */

/**
 * @brief Create a new channel.
 *
 * @return Channel handle, or NULL on allocation failure.
 */
extern xylem_channel_t* xylem_channel_create(void);

/**
 * @brief Destroy the channel and free all buffered messages.
 *
 * Thread-safe. Wakes a parked receiver (if any) with a NULL message.
 * After this call, send() on the same channel is a use-after-free.
 *
 * @param ch  Channel handle.
 */
extern void xylem_channel_destroy(xylem_channel_t* ch);

/**
 * @brief Send a message to the channel. Non-blocking, thread-safe.
 *
 * May be called from any thread and any number of senders in
 * parallel. Does not copy `msg`; the pointer's lifetime is the
 * caller's responsibility until the receiver consumes it.
 *
 * @param ch   Channel handle.
 * @param msg  Opaque message pointer (must be non-NULL).
 *
 * @return 0 on success, -1 on invalid input or allocation failure.
 */
extern int xylem_channel_send(xylem_channel_t* ch, void* msg);

/**
 * @brief Receive the next message. Blocks the caller coroutine.
 *
 * Single-receiver contract: at most one coroutine may be in recv()
 * on a given channel at any time. Violating this aborts the process.
 *
 * @param ch  Channel handle.
 *
 * @return Message pointer, or NULL if the channel has been destroyed.
 */
extern void* xylem_channel_recv(xylem_channel_t* ch);
