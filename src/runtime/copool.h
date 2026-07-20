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

/* Opaque fixed-slot coroutine pool. */
typedef struct copool_s copool_t;

/* Cold-slot initialization callback. It may run concurrently. */
typedef struct copool_slot_ops_s {
    int (*init)(void* ptr, size_t size, void* ud);
    void* ud;
} copool_slot_ops_t;

/**
 * @brief Create a fixed-slot coroutine pool.
 *
 * The required ops structure and init callback are copied by the pool. The
 * caller must keep ops->ud valid until copool_destroy() returns and synchronize
 * access to callback state. init may run concurrently and is called only for a
 * cold slot refilled from the backing arena. Local and shared caches retain hot
 * slot state. Each local pool holds 64 slots; the shared pool holds
 * local_pool_count * 64 slots. Callback size is the page-aligned arena slot
 * size.
 *
 * @param slot_size         Required slot size in bytes, aligned internally.
 * @param local_pool_count  Number of worker-local pools.
 * @param ops               Required cold-slot initializer copied by the pool.
 *
 * @return Pool handle, or NULL for invalid arguments or allocation failure.
 */
extern copool_t* copool_create(
    size_t                   slot_size,
    int32_t                  local_pool_count,
    const copool_slot_ops_t* ops);

/**
 * @brief Destroy a coroutine pool and release its arena.
 *
 * @param pool  Pool handle, or NULL.
 *
 * @note The caller must ensure no pool operations are in flight.
 */
extern void copool_destroy(copool_t* pool);

/**
 * @brief Allocate a committed slot.
 *
 * @param pool         Pool handle.
 * @param local_index  Local cache index, or -1 for the shared path.
 *
 * @return Slot address, or NULL for invalid arguments or allocation failure.
 *
 * @note A nonnegative local_index must not be used concurrently by multiple
 * threads.
 */
extern void* copool_acquire(
    copool_t* pool,
    int32_t local_index);

/**
 * @brief Return a hot slot to a cache or the backing arena.
 *
 * Local and shared caches preserve the slot's committed pages and other hot
 * state. Only slots that spill to the backing arena are decommitted.
 *
 * @param pool         Pool handle, or NULL.
 * @param local_index  Local cache index, or -1 for the shared path.
 * @param ptr          Slot address, or NULL.
 *
 * @note A nonnegative local_index must not be used concurrently by multiple
 * threads.
 */
extern void copool_release(
    copool_t* pool,
    int32_t local_index,
    void* ptr);
