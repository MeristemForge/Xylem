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

#include "deprecated/c11-threads.h"

#include "xylem/xylem-base64.h"
#include "xylem/xylem-bswap.h"
#include "xylem/xylem-heap.h"
#include "xylem/xylem-list.h"
#include "xylem/xylem-queue.h"
#include "xylem/xylem-rbtree.h"
#include "xylem/xylem-ringbuf.h"
#include "xylem/xylem-sha1.h"
#include "xylem/xylem-sha256.h"
#include "xylem/xylem-hmac256.h"
#include "xylem/xylem-aes256.h"
#include "xylem/xylem-stack.h"
#include "xylem/xylem-thrdpool.h"
#include "xylem/xylem-varint.h"
#include "xylem/xylem-waitgroup.h"

#include "xylem/xylem-xheap.h"
#include "xylem/xylem-xlist.h"
#include "xylem/xylem-xqueue.h"
#include "xylem/xylem-xrbtree.h"
#include "xylem/xylem-xstack.h"

#include "xylem/xylem-json.h"
#include "xylem/xylem-gzip.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-loop.h"
#include "xylem/xylem-platform.h"
#include "xylem/xylem-addr.h"
#include "xylem/xylem-tcp.h"
#include "xylem/xylem-udp.h"
#include "xylem/xylem-rudp.h"
#include "xylem/xylem-fec.h"
#include "xylem/xylem-tls.h"
#include "xylem/xylem-dtls.h"
#include "xylem/xylem-utils.h"
#include "xylem/http/xylem-http-common.h"
#include "xylem/http/xylem-http-client.h"
#include "xylem/http/xylem-http-server.h"
#include "xylem/ws/xylem-ws-common.h"
#include "xylem/ws/xylem-ws-client.h"
#include "xylem/ws/xylem-ws-server.h"

/**
 * @brief Initialize the Xylem library.
 *
 * Must be called once before any other xylem_* function. On Windows this
 * calls WSAStartup; on Unix it is a no-op today but may initialize
 * future global state (e.g. OpenSSL).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_startup(void);

/**
 * @brief Clean up the Xylem library.
 *
 * Call once after all Xylem resources have been released. Reverses the
 * effect of xylem_startup().
 */
extern void xylem_cleanup(void);