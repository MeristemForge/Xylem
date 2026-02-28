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

typedef struct xylem_waitgroup_s xylem_waitgroup_t;

/**
 * @brief Create a new waitgroup.
 *
 * Allocates and initializes a waitgroup with an internal counter of zero.
 *
 * @return Pointer to the new waitgroup, or NULL on allocation failure.
 */
extern xylem_waitgroup_t* xylem_waitgroup_create(void);

/**
 * @brief Increment the waitgroup counter.
 *
 * Adds @p delta to the internal counter. Must be called before the
 * corresponding work is dispatched.
 *
 * @param waitgroup  Pointer to the waitgroup.
 * @param delta      Number of work items to add.
 */
extern void xylem_waitgroup_add(xylem_waitgroup_t* waitgroup, size_t delta);

/**
 * @brief Decrement the waitgroup counter by one.
 *
 * Signals that one unit of work has completed. When the counter reaches
 * zero, all threads blocked in xylem_waitgroup_wait() are woken.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_done(xylem_waitgroup_t* waitgroup);

/**
 * @brief Block until the waitgroup counter reaches zero.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_wait(xylem_waitgroup_t* waitgroup);

/**
 * @brief Destroy the waitgroup and free its resources.
 *
 * @param waitgroup  Pointer to the waitgroup.
 */
extern void xylem_waitgroup_destroy(xylem_waitgroup_t* waitgroup);
