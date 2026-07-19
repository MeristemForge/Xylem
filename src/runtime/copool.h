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

#define COPOOL_CACHE_CAP 64

/* Opaque fixed-slot coroutine pool. */
typedef struct copool_s copool_t;

/* Slot lifecycle callbacks copied by copool_create(). */
typedef struct copool_slot_ops_s {
    int (*init)(void* ptr, size_t size, void* ud);
    int (*reset)(void* ptr, size_t size, void* ud);
    void* ud;
} copool_slot_ops_t;

/* Worker-local committed-slot cache. */
typedef struct copool_cache_s {
    void*   slots[COPOOL_CACHE_CAP];
    int32_t count;
} copool_cache_t;

/**
 * @brief Create a fixed-slot coroutine pool.
 *
 * @param slot_size   Maximum allocation size in bytes.
 * @param shared_cap  Maximum number of slots in the shared cache.
 * @param ops         Optional slot lifecycle callbacks copied by the pool.
 *                    init runs for slots obtained from the backing arena;
 *                    reset runs before a slot enters a cache.
 *
 * @return Pool handle, or NULL for invalid arguments or allocation failure.
 */
extern copool_t* copool_create(
    size_t                   slot_size,
    int32_t                  shared_cap,
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
 * @param pool   Pool handle.
 * @param cache  Worker-local cache, or NULL for the shared path.
 * @param size   Requested size, from 1 through the configured slot size.
 *
 * @return Slot address, or NULL for invalid arguments or allocation failure.
 */
extern void* copool_alloc(copool_t* pool, copool_cache_t* cache, size_t size);

/**
 * @brief Return a committed slot to a cache or the backing arena.
 *
 * @param pool   Pool handle, or NULL.
 * @param cache  Worker-local cache, or NULL for the shared path.
 * @param ptr    Slot address, or NULL.
 * @param size   Allocation size, from 1 through the configured slot size.
 */
extern void copool_free(
    copool_t* pool,
    copool_cache_t* cache,
    void* ptr,
    size_t size);
