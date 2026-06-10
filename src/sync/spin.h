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

/**
 * Tiny atomic-flag spin lock.
 *
 * Intended for very short critical sections inside the coroutine
 * sync primitives (waiters-list manipulation in xylem_mutex /
 * xylem_waitgroup). Not a public API and deliberately not exposed
 * through the include/ tree.
 *
 * Callers MUST NOT hold the lock across any operation that can park
 * the coroutine (scheduler_park, iowait_read/write, xylem_mutex_lock
 * on a contested mutex, etc.). Parking a coroutine while holding a
 * spin lock wedges the current scheduler worker until the owning
 * coroutine is rescheduled, which may never happen if the scheduler
 * has no other work to do on this worker.
 *
 * Lifetime: spin_t is meant to be embedded in its owner struct and
 * initialised with spin_init() during that struct's setup. There is
 * no destroy/deinit because the lock owns no external resources.
 *
 * The struct wrapper is deliberate: it keeps the underlying storage
 * opaque, lets the API refuse bare atomic_flag* at the type level,
 * and leaves room to add debug fields (e.g. owner tracking) without
 * touching call sites.
 */

typedef struct spin_s {
    atomic_flag flag;
} spin_t;

/**
 * @brief Initialise an embedded spin lock to the unlocked state.
 *
 * Call once per spin_t before any spin_lock/spin_unlock. Safe to
 * call from any thread as long as no other thread is racing against
 * the same instance (typical pattern: init during owner-struct setup
 * before publishing the owner).
 *
 * @param s  Spin lock storage.
 */
extern void spin_init(spin_t* s);

/**
 * @brief Acquire the spin lock, busy-waiting until granted.
 *
 * Wait is unbounded; each spin iteration emits a CPU pause hint but
 * there is no exponential back-off, so keep critical sections short
 * and bounded. The caller must not park the coroutine while holding
 * the lock (see file-level note).
 *
 * @param s  Spin lock, previously initialised by spin_init().
 */
extern void spin_lock(spin_t* s);

/**
 * @brief Release the spin lock.
 *
 * Must be called by the same thread that holds the lock. Passing
 * an unlocked spin_t is undefined.
 *
 * @param s  Spin lock, currently held by the caller.
 */
extern void spin_unlock(spin_t* s);
