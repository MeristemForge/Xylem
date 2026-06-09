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

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Bounded, lock-free single-producer / single-consumer ring buffer.
 *
 * Exactly one producer thread/coroutine calls push and exactly one
 * consumer thread/coroutine calls pop; concurrent producers or
 * concurrent consumers are undefined. Within that contract push and pop
 * never block and need no lock: the producer owns wpos, the consumer
 * owns rpos, and the lone shared edge is published with one release
 * store and observed with one acquire load.
 *
 * Unlike the MPSC queue there is no "temporarily inconsistent" state: a
 * single producer publishes each slot atomically, so a pop either sees
 * a fully written element or sees the ring as empty -- never a false
 * empty.
 *
 * Elements are stored by value (memcpy of a fixed esz bytes) in a slot
 * array allocated once at init, so the queue performs no per-element
 * allocation. Capacity is fixed at init; a full ring pushes back (push
 * fails) rather than growing. The caller owns the spsc_t itself (it may
 * be embedded or stack-allocated); init allocates the backing slots and
 * deinit frees them.
 */
typedef struct spsc_s {
    uint8_t*           slots; /* cap * esz bytes, owned by the ring */
    size_t             esz;   /* element size in bytes */
    size_t             mask;  /* cap - 1 (cap is a power of two) */
    _Atomic size_t     wpos;  /* producer-owned write index (free running) */
    _Atomic size_t     rpos;  /* consumer-owned read index (free running) */
} spsc_t;

/**
 * @brief Initialize an SPSC ring, allocating its backing storage.
 *
 * Allocates cap * esz bytes of slot storage owned by the ring; release
 * it with spsc_deinit. cap must be a power of two and non-zero, esz
 * must be non-zero.
 *
 * @param q    Pointer to the ring structure to initialize.
 * @param cap  Number of element slots; must be a power of two.
 * @param esz  Element size in bytes; must be non-zero.
 *
 * @return 0 on success, -1 on invalid arguments or allocation failure.
 */
extern int spsc_init(spsc_t* q, size_t cap, size_t esz);

/**
 * @brief Release the ring's backing storage.
 *
 * Frees the slots allocated by spsc_init. Does not free the spsc_t
 * itself (the caller owns it). Accepts NULL.
 *
 * @param q  Pointer to the ring, or NULL.
 */
extern void spsc_deinit(spsc_t* q);

/**
 * @brief Push one element by value (producer side only).
 *
 * Copies esz bytes from elem into the next free slot.
 *
 * @param q     Pointer to the ring.
 * @param elem  Element to copy in; must be non-NULL.
 *
 * @return 0 on success, -1 if the ring is full.
 */
extern int spsc_push(spsc_t* q, const void* elem);

/**
 * @brief Pop one element by value (consumer side only).
 *
 * Copies esz bytes of the oldest element into out.
 *
 * @param q    Pointer to the ring.
 * @param out  Destination of at least esz bytes; must be non-NULL.
 *
 * @return 0 on success, -1 if the ring is empty.
 */
extern int spsc_pop(spsc_t* q, void* out);

/**
 * @brief Number of elements currently queued (best-effort snapshot).
 *
 * @param q  Pointer to the ring.
 *
 * @return Element count at the moment of the call.
 */
extern size_t spsc_len(const spsc_t* q);

/**
 * @brief Total element capacity of the ring.
 *
 * @param q  Pointer to the ring.
 *
 * @return Capacity in elements.
 */
extern size_t spsc_cap(const spsc_t* q);

/**
 * @brief Whether the ring appears empty (best-effort snapshot).
 *
 * @param q  Pointer to the ring.
 *
 * @return true if no elements are queued at the moment of the call.
 */
extern bool spsc_empty(const spsc_t* q);

/**
 * @brief Whether the ring appears full (best-effort snapshot).
 *
 * @param q  Pointer to the ring.
 *
 * @return true if the ring holds cap elements at the moment of the call.
 */
extern bool spsc_full(const spsc_t* q);
