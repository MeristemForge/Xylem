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

#include "xylem/xylem-error.h"

const char* xylem_err_tostring(xylem_err_t err) {
    switch (err) {
    case XYLEM_ERR_NONE:         return "none";
    case XYLEM_ERR_TIMEOUT:      return "timeout";
    case XYLEM_ERR_PEER_RESET:   return "peer reset";
    case XYLEM_ERR_CONNREFUSED:  return "connection refused";
    case XYLEM_ERR_CLOSED:       return "closed";
    case XYLEM_ERR_ADDRINUSE:    return "address in use";
    case XYLEM_ERR_ADDRNOTAVAIL: return "address not available";
    case XYLEM_ERR_NETUNREACH:   return "network unreachable";
    case XYLEM_ERR_HOSTUNREACH:  return "host unreachable";
    case XYLEM_ERR_NOMEM:        return "out of memory";
    case XYLEM_ERR_PEER_CLOSED:  return "peer closed";
    case XYLEM_ERR_UNKNOWN:      return "unknown error";
    case XYLEM_ERR_TLS:          return "tls error";
    case XYLEM_ERR_DTLS:         return "dtls error";
    }
    return "unknown error";
}
