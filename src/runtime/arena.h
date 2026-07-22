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

/* Opaque fixed-slot virtual-memory arena. */
typedef struct arena_s arena_t;

/**
 * @brief Create a fixed-slot virtual-memory arena.
 *
 * The slot size is rounded up to the system page size and must not exceed
 * 1 MiB after alignment.
 *
 * @param slot_size  Requested slot size in bytes.
 *
 * @return Arena handle, or NULL for an invalid size or allocation failure.
 */
extern arena_t* arena_create(size_t slot_size);

/**
 * @brief Destroy an arena and release all reservations.
 *
 * @param arena  Arena handle, or NULL.
 *
 * @note The caller must ensure no arena operations are in flight.
 */
extern void arena_destroy(arena_t* arena);

/**
 * @brief Allocate fixed-size fresh slots.
 *
 * Returned addresses are fully decommitted and must be initialized before any
 * access. At most one region is added per call.
 *
 * @param arena  Arena handle.
 * @param slots  Output array with capacity for count pointers.
 * @param count  Maximum number of slots to allocate.
 *
 * @return Number of fresh slot addresses written to slots, from 0 through
 *         count.
 */
extern int arena_alloc(arena_t* arena, void** slots, int count);

/**
 * @brief Decommit and return slots to an arena.
 *
 * Only slots decommitted successfully re-enter the free list. The slots array
 * may be compacted in place so successful addresses occupy its leading entries.
 *
 * @param arena  Arena handle.
 * @param slots  Mutable array of complete slot addresses from arena_alloc().
 * @param count  Number of slots to return.
 */
extern void arena_free(arena_t* arena, void** slots, int count);
