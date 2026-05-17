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
 * @brief Cross-platform IO error codes.
 *
 * Shared by network (TCP, UDP, TLS, RUDP) and serial modules.
 * Platform-specific error codes are translated into these at the
 * point of failure so user code does not need platform conditionals.
 */
typedef enum xylem_err_e {
    XYLEM_ERR_NONE         = 0,  /*< No error. */
    XYLEM_ERR_TIMEOUT      = 1,  /*< Deadline exceeded or operation timed out. */
    XYLEM_ERR_PEER_RESET    = 2,  /*< Connection reset by peer. */
    XYLEM_ERR_CONNREFUSED  = 3,  /*< Connection refused by peer. */
    XYLEM_ERR_CLOSED       = 4,  /*< Handle was closed locally. */
    XYLEM_ERR_ADDRINUSE    = 5,  /*< Address already in use. */
    XYLEM_ERR_ADDRNOTAVAIL = 6,  /*< Address not available. */
    XYLEM_ERR_NETUNREACH   = 7,  /*< Network unreachable. */
    XYLEM_ERR_HOSTUNREACH  = 8,  /*< Host unreachable. */
    XYLEM_ERR_NOMEM        = 9,  /*< Out of memory or file descriptors. */
    XYLEM_ERR_PEER_CLOSED          = 10, /*< Peer closed gracefully (read returned 0). */
    XYLEM_ERR_UNKNOWN      = 11, /*< Unmapped platform error. */
    XYLEM_ERR_TLS          = 12, /*< TLS/SSL layer error. */
    XYLEM_ERR_DTLS         = 13, /*< DTLS layer error. */
} xylem_err_t;

/**
 * @brief Return a short human-readable string for an error code.
 *
 * The returned pointer is to a static string; do not free it.
 *
 * @param err  Error code.
 *
 * @return Null-terminated description, e.g. "timeout".
 */
extern const char* xylem_err_tostring(xylem_err_t err);
