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

#define COPOOL_LOCAL_DEFAULT_CAP 64

typedef struct copool_local_s  copool_local_t;
typedef struct copool_shared_s copool_shared_t;

typedef enum copool_slot_state_e {
    COPOOL_SLOT_FRESH,
    COPOOL_SLOT_REUSABLE,
} copool_slot_state_t;

typedef struct copool_slot_s {
    void*               ptr;
    copool_slot_state_t state;
} copool_slot_t;

/**
 * @brief Create a worker-local coroutine slot pool.
 *
 * @param capacity  Maximum number of slots, or 0 for the default of 64.
 *
 * @return Pool handle, or NULL for an invalid capacity or allocation failure.
 */
extern copool_local_t* copool_local_create(int capacity);

/**
 * @brief Destroy a worker-local coroutine slot pool.
 *
 * Stored slot addresses are discarded; the pool does not own slot memory.
 *
 * @param pool  Pool handle, or NULL.
 */
extern void copool_local_destroy(copool_local_t* pool);

/**
 * @brief Return the capacity of a worker-local pool.
 *
 * @param pool  Pool handle, or NULL.
 *
 * @return Slot capacity, or 0 for NULL.
 */
extern int copool_local_capacity(const copool_local_t* pool);

/**
 * @brief Take up to count slots from a worker-local pool.
 *
 * Slots are removed in LIFO order. A local pool may be accessed only by its
 * owning worker.
 *
 * @param pool   Pool handle.
 * @param slots  Output array with capacity for count entries.
 * @param count  Maximum number of slots to take.
 *
 * @return Number of slots taken, from 0 through count.
 */
extern int copool_local_take(
    copool_local_t* pool,
    copool_slot_t*  slots,
    int             count);

/**
 * @brief Put up to count slots into a worker-local pool.
 *
 * A local pool may be accessed only by its owning worker.
 *
 * @param pool   Pool handle.
 * @param slots  Slot entries to store.
 * @param count  Number of entries available in slots.
 *
 * @return Number of slots stored, from 0 through count.
 */
extern int copool_local_put(
    copool_local_t*      pool,
    const copool_slot_t* slots,
    int                  count);

/**
 * @brief Create an unbounded shared coroutine slot pool.
 *
 * @return Pool handle, or NULL on allocation failure.
 */
extern copool_shared_t* copool_shared_create(void);

/**
 * @brief Destroy a shared coroutine slot pool.
 *
 * Stored slot addresses are discarded; the pool does not own slot memory.
 *
 * @param pool  Pool handle, or NULL.
 *
 * @note The caller must ensure no pool operations are in flight.
 */
extern void copool_shared_destroy(copool_shared_t* pool);

/**
 * @brief Take up to count slots from the shared pool.
 *
 * Slots are removed in LIFO order. This operation is thread-safe.
 *
 * @param pool   Pool handle.
 * @param slots  Output array with capacity for count entries.
 * @param count  Maximum number of slots to take.
 *
 * @return Number of slots taken, from 0 through count.
 */
extern int copool_shared_take(
    copool_shared_t* pool,
    copool_slot_t*   slots,
    int              count);

/**
 * @brief Put up to count slots into the shared pool.
 *
 * A metadata node is allocated for each stored slot. This operation is
 * thread-safe and may store fewer than count entries on allocation failure.
 *
 * @param pool   Pool handle.
 * @param slots  Slot entries to store.
 * @param count  Number of entries available in slots.
 *
 * @return Number of slots stored, from 0 through count.
 */
extern int copool_shared_put(
    copool_shared_t*     pool,
    const copool_slot_t* slots,
    int                  count);
