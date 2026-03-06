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

#include "xylem.h"

typedef struct xylem_ringbuf_s xylem_ringbuf_t;

/**
 * @brief Create a ring buffer.
 *
 * Allocates a ring buffer whose internal capacity is rounded down to the
 * largest power-of-two number of entries that fit in bufsize bytes.
 *
 * @param esize    Size of each entry in bytes. Must be > 0.
 * @param bufsize  Total buffer size in bytes. Must be >= esize.
 *
 * @return Pointer to the new ring buffer, or NULL on failure.
 */
extern xylem_ringbuf_t* xylem_ringbuf_create(size_t esize, size_t bufsize);

/**
 * @brief Destroy a ring buffer and free its memory.
 *
 * @param ring  Pointer to the ring buffer.
 */
extern void xylem_ringbuf_destroy(xylem_ringbuf_t* ring);

/**
 * @brief Check whether the ring buffer is full.
 *
 * @param ring  Pointer to the ring buffer.
 *
 * @return true if no space remains, false otherwise.
 */
extern bool xylem_ringbuf_full(xylem_ringbuf_t* ring);

/**
 * @brief Check whether the ring buffer is empty.
 *
 * @param ring  Pointer to the ring buffer.
 *
 * @return true if no entries are stored, false otherwise.
 */
extern bool xylem_ringbuf_empty(xylem_ringbuf_t* ring);

/**
 * @brief Return the number of entries currently in the ring buffer.
 *
 * @param ring  Pointer to the ring buffer.
 *
 * @return Number of entries.
 */
extern size_t xylem_ringbuf_len(xylem_ringbuf_t* ring);

/**
 * @brief Return the total capacity of the ring buffer in entries.
 *
 * @param ring  Pointer to the ring buffer.
 *
 * @return Capacity (always a power of two).
 */
extern size_t xylem_ringbuf_cap(xylem_ringbuf_t* ring);

/**
 * @brief Return the number of entries that can be written before the buffer is full.
 *
 * @param ring  Pointer to the ring buffer.
 *
 * @return Available entry count.
 */
extern size_t xylem_ringbuf_avail(xylem_ringbuf_t* ring);

/**
 * @brief Write entries into the ring buffer.
 *
 * Copies up to entry_count entries from buf into the ring buffer.
 * If fewer slots are available, only the available amount is written.
 *
 * @param ring         Pointer to the ring buffer.
 * @param buf          Source data to write.
 * @param entry_count  Number of entries to write.
 *
 * @return Number of entries actually written.
 */
extern size_t xylem_ringbuf_write(xylem_ringbuf_t* ring, const void* buf, size_t entry_count);

/**
 * @brief Read and consume entries from the ring buffer.
 *
 * Copies up to entry_count entries from the ring buffer into buf
 * and advances the read position.
 *
 * @param ring         Pointer to the ring buffer.
 * @param buf          Destination buffer.
 * @param entry_count  Maximum number of entries to read.
 *
 * @return Number of entries actually read.
 */
extern size_t xylem_ringbuf_read(xylem_ringbuf_t* ring, void* buf, size_t entry_count);

/**
 * @brief Peek entries from the ring buffer without consuming them.
 *
 * Copies up to entry_count entries from the ring buffer into buf
 * but does NOT advance the read position. The data remains available
 * for subsequent read or peek calls.
 *
 * @param ring         Pointer to the ring buffer.
 * @param buf          Destination buffer.
 * @param entry_count  Maximum number of entries to peek.
 *
 * @return Number of entries actually copied.
 */
extern size_t xylem_ringbuf_peek(xylem_ringbuf_t* ring, void* buf, size_t entry_count);


/**
 * @brief Peek entries from the ring buffer without consuming them.
 *
 * Copies up to entry_count entries from the ring buffer into buf
 * but does NOT advance the read position. The data remains available
 * for subsequent read or peek calls.
 *
 * @param ring         Pointer to the ring buffer.
 * @param buf          Destination buffer.
 * @param entry_count  Maximum number of entries to peek.
 *
 * @return Number of entries actually copied.
 */
extern size_t xylem_ringbuf_peek(xylem_ringbuf_t* ring, void* buf, size_t entry_count);

