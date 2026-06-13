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

#include "xylem/net/http/xylem-http-router.h"

#include <stdbool.h>

typedef struct xylem_http_fileserver_opts_s {
    const char* index_file;  /* Default "index.html", NULL to disable. */
    bool        dir_listing; /* true = show directory listing. */
} xylem_http_fileserver_opts_t;

/**
 * @brief Register a static file server on a router.
 *
 * @note [COROUTINE-ONLY]
 *
 * Serves files from root_dir under url_prefix. Includes MIME type detection,
 * ETag/Range support via xylem_http_serve_content, and path traversal protection.
 *
 * @param router      Router handle.
 * @param url_prefix  URL path prefix (e.g. "/static").
 * @param root_dir    Filesystem root directory (e.g. "./public").
 * @param opts        Options, or NULL for defaults (index.html enabled, no dir listing).
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_http_fileserver(
    xylem_http_router_t*              router,
    const char*                       url_prefix,
    const char*                       root_dir,
    const xylem_http_fileserver_opts_t* opts);
