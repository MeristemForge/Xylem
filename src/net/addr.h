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

#include "platform/platform-socket.h"

#define ADDR_MAXHOST 46

typedef struct loop_s      loop_t;
typedef struct thrdpool_s  thrdpool_t;
typedef struct addr_resolve_s addr_resolve_t;

typedef struct addr_s {
    struct sockaddr_storage storage;
} addr_t;

typedef void (*addr_resolve_fn_t)(addr_t* addrs, size_t count,
                                  int status, void* userdata);

extern int addr_pton(const char* host, uint16_t port, addr_t* addr);

extern int addr_ntop(const addr_t* addr,
                     char* host, size_t hostlen, uint16_t* port);

extern addr_resolve_t* addr_resolve(loop_t* loop,
                                    thrdpool_t* pool,
                                    const char* host,
                                    uint16_t port,
                                    addr_resolve_fn_t cb,
                                    void* userdata);

extern void addr_resolve_cancel(addr_resolve_t* req);
