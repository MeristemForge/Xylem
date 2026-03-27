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

typedef struct xylem_thrdpool_s xylem_thrdpool_t;

/**
 * @brief Create a thread pool with the specified number of worker threads.
 *
 * Allocates and initializes a thread pool. Each worker thread starts
 * immediately and waits for jobs to be posted.
 *
 * @param nthrds  Number of worker threads to spawn.
 * @return Pointer to the new thread pool, or NULL on allocation failure.
 */
extern xylem_thrdpool_t* xylem_thrdpool_create(int nthrds);


/**
 * @brief Post a job to the thread pool for asynchronous execution.
 *
 * Enqueues a function and its argument for execution by the next available
 * worker thread. The job is dispatched via a condition variable signal.
 *
 * @param pool     Pointer to the thread pool.
 * @param routine  Function to execute. Called with @p arg as its parameter.
 * @param arg      Opaque argument passed to @p routine.
 */
extern void xylem_thrdpool_post(xylem_thrdpool_t* restrict pool, void (*routine)(void*), void* arg);

/**
 * @brief Destroy the thread pool and release all resources.
 *
 * Signals all worker threads to stop, joins them, frees any remaining
 * unprocessed jobs in the queue, and destroys synchronization primitives.
 *
 * @param pool  Pointer to the thread pool.
 */
extern void xylem_thrdpool_destroy(xylem_thrdpool_t* restrict pool);
