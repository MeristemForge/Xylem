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

#include <stdint.h>

typedef struct xylem_channel_s xylem_channel_t;

/**
 * MPSC channel: many senders, single receiver.
 *
 * Threading:
 *   - send(), close(), destroy() are safe from any thread.
 *   - recv() must be called from a coroutine. Only one coroutine
 *     may recv on a given channel; concurrent recv aborts.
 */

/**
 * @brief Create a new channel.
 *
 * @return Channel handle, or NULL on allocation failure.
 */
extern xylem_channel_t* xylem_channel_create(void);

/**
 * @brief Destroy the channel, releasing its memory.
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
 * Aborts if the channel is closed.
 *
 * @param ch   Channel handle.
 * @param msg  Opaque message pointer (must be non-NULL).
 *
 * @return 0 on success, -1 on invalid input or allocation failure.
 */
extern int xylem_channel_send(xylem_channel_t* ch, void* msg);

/**
 * @brief Receive the next message. Blocks the calling coroutine.
 *
 * Concurrent recv from multiple coroutines aborts.
 *
 * @param ch  Channel handle.
 *
 * @return Message pointer, or NULL if the channel is closed and empty.
 */
extern void* xylem_channel_recv(xylem_channel_t* ch);

/**
 * @brief Receive the next message with a timeout. Blocks the
 *        calling coroutine until a message arrives, the channel
 *        closes, or the timeout elapses.
 *
 * Concurrent recv from multiple coroutines aborts (same single-
 * receiver contract as recv).
 *
 * @param ch          Channel handle.
 * @param timeout_ms  Relative timeout in milliseconds. 0 means no
 *                    timeout, identical to recv().
 *
 * @return Message pointer, or NULL if the timeout elapsed, or the
 *         channel is closed and empty. The NULL cases are not
 *         distinguished; callers needing the reason should track it
 *         out of band.
 */
extern void* xylem_channel_recv_timeout(
    xylem_channel_t* ch, uint64_t timeout_ms);
