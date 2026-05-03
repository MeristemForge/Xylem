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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && !defined(_XYLEM_SSIZE_T)
#define _XYLEM_SSIZE_T
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#ifndef XYLEM_ADDR_MAXHOST
#define XYLEM_ADDR_MAXHOST 46
#endif

typedef struct xylem_tcp_conn_s     xylem_tcp_conn_t;
typedef struct xylem_tcp_listener_s xylem_tcp_listener_t;

typedef struct xylem_tcp_opts_s {
    bool disable_mss_clamp;
} xylem_tcp_opts_t;

typedef struct xylem_tcp_frame_opts_s {
    uint32_t header_size;
    uint32_t field_offset;
    uint32_t field_size;
    int32_t  adjustment;
    bool     big_endian;
} xylem_tcp_frame_opts_t;

extern xylem_tcp_listener_t* xylem_tcp_listen(const char* host,
                                              uint16_t port,
                                              xylem_tcp_opts_t* opts);

extern xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* ln);

extern void xylem_tcp_close_listener(xylem_tcp_listener_t* ln);

extern xylem_tcp_conn_t* xylem_tcp_dial(const char* host,
                                        uint16_t port,
                                        xylem_tcp_opts_t* opts);

extern xylem_tcp_conn_t* xylem_tcp_dial_timeout(const char* host,
                                                uint16_t port,
                                                xylem_tcp_opts_t* opts,
                                                uint64_t ms);

extern ssize_t xylem_tcp_recv(xylem_tcp_conn_t* tcp,
                              void* buf, size_t len);

extern ssize_t xylem_tcp_recv_timeout(xylem_tcp_conn_t* tcp,
                                      void* buf, size_t len,
                                      uint64_t ms);

extern int xylem_tcp_recv_exact(xylem_tcp_conn_t* tcp,
                                void* buf, size_t len);

extern int xylem_tcp_send(xylem_tcp_conn_t* tcp,
                          const void* data, size_t len);

extern void* xylem_tcp_recv_frame(xylem_tcp_conn_t* tcp,
                                  xylem_tcp_frame_opts_t* opts,
                                  size_t* out_len);

extern int xylem_tcp_send_frame(xylem_tcp_conn_t* tcp,
                                xylem_tcp_frame_opts_t* opts,
                                const void* data, size_t len);

extern ssize_t xylem_tcp_recv_line(xylem_tcp_conn_t* tcp,
                                   char* buf, size_t max);

extern void xylem_tcp_close(xylem_tcp_conn_t* tcp);

extern int xylem_tcp_get_error(xylem_tcp_conn_t* tcp);

extern int xylem_tcp_remote_addr(xylem_tcp_conn_t* tcp,
                                 char host[XYLEM_ADDR_MAXHOST],
                                 uint16_t* port);

extern void* xylem_tcp_get_userdata(xylem_tcp_conn_t* tcp);
extern void  xylem_tcp_set_userdata(xylem_tcp_conn_t* tcp, void* ud);

extern void* xylem_tcp_listener_get_userdata(xylem_tcp_listener_t* ln);
extern void  xylem_tcp_listener_set_userdata(xylem_tcp_listener_t* ln,
                                             void* ud);
