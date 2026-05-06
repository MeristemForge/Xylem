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

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_udp_s xylem_udp_t;

typedef struct xylem_udp_handler_s {
    void (*on_read)(xylem_udp_t* udp, void* data, size_t len,
                    const char* host, uint16_t port);
    void (*on_close)(xylem_udp_t* udp, int err, const char* errmsg);
} xylem_udp_handler_t;

extern xylem_udp_t* xylem_udp_listen(const char* host,
                                     uint16_t port,
                                     xylem_udp_handler_t* handler);

extern xylem_udp_t* xylem_udp_dial(const char* host,
                                    uint16_t port,
                                    xylem_udp_handler_t* handler);

extern int xylem_udp_send(xylem_udp_t* udp,
                          const char* host, uint16_t port,
                          const void* data, size_t len);

extern void xylem_udp_close(xylem_udp_t* udp);

extern void xylem_udp_ref(xylem_udp_t* udp);
extern void xylem_udp_unref(xylem_udp_t* udp);

extern void* xylem_udp_get_userdata(xylem_udp_t* udp);
extern void  xylem_udp_set_userdata(xylem_udp_t* udp, void* ud);
