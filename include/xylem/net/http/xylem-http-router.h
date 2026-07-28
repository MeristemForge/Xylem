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

#include "xylem/net/http/xylem-http.h"

typedef struct xylem_http_router_s xylem_http_router_t;
typedef struct xylem_http_next_s   xylem_http_next_t;

typedef void (*xylem_http_middleware_fn_t)(
    xylem_http_writer_t* writer,
    xylem_http_req_t*    req,
    xylem_http_next_t*   next,
    void*                userdata);

/**
 * @brief Create a new router.
 *
 * @note [COROUTINE-ONLY]
 *
 * @return Router handle, or NULL on failure.
 */
extern xylem_http_router_t* xylem_http_router_create(void);

/**
 * @brief Destroy a router and all registered routes. NULL-safe.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param router  Router handle.
 */
extern void xylem_http_router_destroy(xylem_http_router_t* router);

/**
 * @brief Register a route handler.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param router    Router handle.
 * @param method    HTTP method (e.g. "GET", "POST").
 * @param pattern   Route pattern with named (":id") or catch-all
 *                  ("*filepath") params, e.g. "/users/:id".
 * @param handler   Handler function.
 * @param userdata  Passed to handler.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_router_handle(
    xylem_http_router_t*    router,
    const char*             method,
    const char*             pattern,
    xylem_http_handler_fn_t handler,
    void*                   userdata);

/**
 * @brief Register a global middleware (onion model).
 *
 * @note [COROUTINE-ONLY]
 *
 * Middleware executes in registration order. Call xylem_http_next_run() to
 * pass control to the next middleware or route handler. Code after the call
 * runs on the way back out. Not calling it short-circuits the chain.
 *
 * @param router      Router handle.
 * @param middleware  Middleware handler function.
 * @param userdata    Passed to middleware.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_router_use(
    xylem_http_router_t*       router,
    xylem_http_middleware_fn_t middleware,
    void*                      userdata);

/**
 * @brief Create a route group with a shared prefix.
 *
 * @note [COROUTINE-ONLY]
 *
 * The group shares the parent's middleware and prepends prefix to all routes.
 * The returned group is owned by the parent and freed with it.
 *
 * @param parent  Parent router.
 * @param prefix  URL prefix for all routes in this group.
 *
 * @return Group handle, or NULL on error.
 */
extern xylem_http_router_t* xylem_http_router_group(
    xylem_http_router_t* parent,
    const char*          prefix);

/**
 * @brief Advance to the next middleware or the final route handler.
 *
 * @note [COROUTINE-ONLY]
 *
 * Call this inside a middleware to pass control inward. Code after this
 * call executes on the way back out (post-handler phase, onion model).
 * Not calling next() aborts the chain (short-circuit).
 *
 * A next handle may be run at most once. Additional calls are ignored.
 *
 * @param next  Next middleware handle.
 */
extern void xylem_http_next_run(xylem_http_next_t* next);

/**
 * @brief Get a path parameter value from a matched route.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param req   Request handle (must be inside a router handler).
 * @param name  Parameter name (e.g. "id" for pattern ":id").
 *
 * @return Parameter value, or NULL if not found.
 */
extern const char* xylem_http_router_param(
    const xylem_http_req_t* req,
    const char*             name);

/**
 * @brief Get the router dispatch function for use with xylem_http_listen.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param router  Router handle.
 *
 * @return Handler function pointer.
 */
extern xylem_http_handler_fn_t xylem_http_router_handler(
    xylem_http_router_t* router);

/**
 * @brief Get the router userdata for use with xylem_http_listen.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param router  Router handle.
 *
 * @return Userdata pointer (the router itself).
 */
extern void* xylem_http_router_handler_userdata(
    xylem_http_router_t* router);

/**
 * @brief Set a custom 404 Not Found handler.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param router   Router handle.
 * @param handler  Handler for unmatched routes.
 * @param userdata Passed to handler.
 */
extern void xylem_http_router_set_not_found(
    xylem_http_router_t*    router,
    xylem_http_handler_fn_t handler,
    void*                   userdata);
