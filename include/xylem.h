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

/* crypto */
#include "xylem/crypto/xylem-sha1.h"
#include "xylem/crypto/xylem-sha256.h"
#include "xylem/crypto/xylem-hmac256.h"
#include "xylem/crypto/xylem-aes256.h"

/* encoding */
#include "xylem/encoding/xylem-base64.h"
#include "xylem/encoding/xylem-bswap.h"
#include "xylem/encoding/xylem-varint.h"
#include "xylem/encoding/xylem-json.h"
#include "xylem/encoding/xylem-gzip.h"
#include "xylem/encoding/xylem-fec.h"

/* sync */
#include "xylem/sync/xylem-waitgroup.h"
#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"

/* runtime */
#include "xylem/runtime/xylem-runtime.h"

/* container */
#include "xylem/container/xylem-ringbuf.h"
#include "xylem/container/xylem-list.h"
#include "xylem/container/xylem-stack.h"
#include "xylem/container/xylem-queue.h"
#include "xylem/container/xylem-heap.h"
#include "xylem/container/xylem-rbtree.h"

/* core */
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-serial.h"

/* net */
#include "xylem/net/xylem-tcp.h"
#include "xylem/net/xylem-udp.h"
#include "xylem/net/xylem-rudp.h"
#include "xylem/net/xylem-uds.h"
#ifdef XYLEM_ENABLE_TLS
#include "xylem/net/xylem-tls.h"
#include "xylem/net/xylem-dtls.h"
#endif
#ifdef XYLEM_ENABLE_HTTP
#include "xylem/net/http/xylem-http-common.h"
#include "xylem/net/http/xylem-http-client.h"
#include "xylem/net/http/xylem-http-server.h"
#endif
#ifdef XYLEM_ENABLE_WS
#include "xylem/net/ws/xylem-ws-common.h"
#include "xylem/net/ws/xylem-ws-client.h"
#include "xylem/net/ws/xylem-ws-server.h"
#endif
