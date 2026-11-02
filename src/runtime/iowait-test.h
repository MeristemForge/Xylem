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

#include "iowait.h"

#include <stdbool.h>
#include <stdint.h>

/* Available only in builds with testing enabled. */
/**
 * @brief Read a direction's waiter state in a test build.
 */
extern uintptr_t iowait_test_get_waiter(iowait_t* w, bool write);

/**
 * @brief Inject a read reservation without depending on the handle layout.
 */
extern void iowait_test_set_read_waiter(iowait_t* w, uintptr_t state);

/**
 * @brief Inspect poll flags in a test build.
 */
extern uint32_t iowait_test_get_poll_info(iowait_t* w);

/**
 * @brief Inject or clear poll flags for state transition tests.
 */
extern void iowait_test_update_poll_info(iowait_t* w, uint32_t set, uint32_t clear);

/**
 * @brief Capture the poller token for stale event tests.
 */
extern void* iowait_test_event_data(iowait_t* w);
