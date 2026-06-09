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

/**
 * @brief Create a cookie jar.
 *
 * Coroutine-only.
 *
 * @return Cookie jar handle, or NULL on failure.
 */
extern xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);

/**
 * @brief Destroy a cookie jar. NULL-safe.
 *
 * Coroutine-only.
 *
 * @param jar  Cookie jar handle.
 */
extern void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);

/**
 * @brief Manually set a cookie in the jar.
 *
 * Coroutine-only.
 *
 * @param jar    Cookie jar handle.
 * @param url    URL to associate the cookie with (for domain/path matching).
 * @param name   Cookie name.
 * @param value  Cookie value.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_cookie_jar_set(
    xylem_http_cookie_jar_t* jar,
    const char*              url,
    const char*              name,
    const char*              value);

/**
 * @brief Get a cookie value from the jar.
 *
 * Coroutine-only.
 *
 * @param jar   Cookie jar handle.
 * @param url   URL to match against (domain/path).
 * @param name  Cookie name.
 *
 * @return Cookie value, or NULL if not found.
 */
extern const char* xylem_http_cookie_jar_get(
    const xylem_http_cookie_jar_t* jar,
    const char*                    url,
    const char*                    name);
