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

#include "xylem/xylem-logger.h"

#include "runtime/minicoro/minicoro.h"

#include <stdlib.h>

/**
 * Abort if the caller is not running on a coroutine. Use at the top of
 * any API that may park the calling coroutine (I/O, sleep, blocking
 * lock/wait): such an API on a non-coroutine thread cannot suspend and
 * would corrupt the scheduler, so fail fast at the misuse site rather
 * than at some later contended park. Do NOT use on an any-thread-safe
 * low-level path (an internal close that only wakes, unlock, spawn, schedule,
 * or iowait deadline setter). A public protocol API may still require a
 * coroutine for handle ownership even when its low-level operation cannot
 * park.
 *
 * mod and api are string literals naming the module and the API for the
 * diagnostic, e.g. RUNTIME_REQUIRE_COROUTINE("tls", "tls_dial").
 */
#define RUNTIME_REQUIRE_COROUTINE(mod, api)                                 \
    do {                                                                    \
        if (!mco_running()) {                                               \
            xylem_loge("<%s> %s must be called from a coroutine on a "      \
                       "scheduler worker; aborting",                        \
                       (mod), (api));                                       \
            abort();                                                        \
        }                                                                   \
    } while (0)
