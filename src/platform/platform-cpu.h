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

/**
 * platform_cpu_relax(): CPU pause/yield hint for busy-wait loops.
 *
 * Kept header-only and inline because it sits in the innermost iteration
 * of tight spin loops (spin_lock, the xylem_mutex adaptive spin); an
 * out-of-line call would cost more than the single pause instruction it
 * emits. The hint does not yield the OS thread, it only tells the core we
 * are spinning so it can save power and release shared SMT resources.
 * Per-platform backends provide the actual definition.
 */
#if defined(_WIN32)
#include "platform/win/platform-cpu.h"
#endif

#if defined(__linux__) || defined(__APPLE__)
#include "platform/unix/platform-cpu.h"
#endif

