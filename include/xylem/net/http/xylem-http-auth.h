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

#include <stdbool.h>

/* ---- Client-side: build Authorization header value ---- */

/**
 * @brief Compute the buffer size needed for xylem_http_basic_auth().
 *
 * @param ulen  Length of the username.
 * @param plen  Length of the password.
 *
 * @return Required buffer size including null terminator.
 */
extern int xylem_http_basic_auth_size(int ulen, int plen);

/**
 * @brief Build a Basic Authorization header value into caller-provided buffer.
 *
 * @param user    Username (null-terminated).
 * @param pass    Password (null-terminated).
 * @param buf     Output buffer.
 * @param buflen  Size of buf in bytes.
 *
 * @return Number of bytes written (excluding null terminator), or -1 on error.
 */
extern int xylem_http_basic_auth(
    const char* user,
    const char* pass,
    char*       buf,
    int         buflen);

/* ---- Server-side: Basic Auth middleware ---- */

typedef bool (*xylem_http_auth_check_fn_t)(const char* user,
                                           const char* pass,
                                           void*       userdata);

typedef struct {
    xylem_http_auth_check_fn_t check;    /* Return true to allow. */
    void*                      userdata; /* Passed to check. */
    const char*                realm;    /* NULL = "Restricted". */
} xylem_http_basic_auth_cfg_t;

/**
 * @brief Basic Auth middleware.
 *
 * Pass a pointer to xylem_http_basic_auth_cfg_t as userdata:
 *   xylem_http_router_use(router, xylem_http_basic_auth_middleware, &cfg);
 *
 * On success calls next(). On failure responds 401 with WWW-Authenticate.
 */
extern void xylem_http_basic_auth_middleware(xylem_http_res_t* res,
                                            xylem_http_req_t* req,
                                            void*             userdata);
