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

#include "xylem/net/xylem-dtls.h"
#include "xylem/sync/xylem-mutex.h"
#include "xylem/sync/xylem-channel.h"

#include "container/rbtree.h"
#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/scheduler.h"
#include "thrds.h"

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define DTLS_DEFAULT_TIMEOUT_MS  30000
#define DTLS_COOKIE_SIZE         32
#define DTLS_INBOX_CAP           64
#define DTLS_PKT_BUF_SIZE        1500

typedef struct _dtls_dgram_s {
    size_t len;
    char   data[];
} _dtls_dgram_t;

struct xylem_dtls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
    uint8_t  cookie_secret[DTLS_COOKIE_SIZE];
};

struct xylem_dtls_conn_s {
    SSL*                    ssl;
    addr_t                  peer_addr;
    char                    alpn[256];
    _Atomic bool            closed;
    _Atomic int32_t         refcnt;
    bool                    handshake_done;

    /* client-side only */
    iowait_t*               waiter;
    platform_sock_t          fd;

    /* server-side only */
    xylem_channel_t*         inbox;
    _Atomic int32_t          inbox_len;
    BIO*                     read_bio;
    BIO*                     write_bio;
    sched_timer_t*           retransmit_timer;
    sched_timer_t*           handshake_timer;
    xylem_dtls_listener_t*   listener;
    rbtree_node_t            server_node;
    uint64_t                 rd_deadline_ms;
};

struct xylem_dtls_listener_s {
    platform_sock_t       fd;
    iowait_t*             waiter;
    xylem_dtls_ctx_t*     ctx;
    xylem_dtls_opts_t     opts;
    rbtree_t              sessions;
    mtx_t                 sessions_mtx;
    xylem_mutex_t*        write_mu;
    scheduler_t*          sched;

    xylem_channel_t*      accept_ch;

    _Atomic bool          closed;
    _Atomic int32_t       refcnt;
};

extern void dtls_conn_ref(xylem_dtls_conn_t* dtls);
extern void dtls_conn_unref(xylem_dtls_conn_t* dtls);

extern void dtls_listener_ref(xylem_dtls_listener_t* ln);
extern void dtls_listener_unref(xylem_dtls_listener_t* ln);

extern void dtls_inbox_push(xylem_dtls_conn_t* dtls, _dtls_dgram_t* dgram);

extern xylem_dtls_conn_t* dtls_find_session(xylem_dtls_listener_t* ln,
                                             addr_t* addr);
extern void dtls_sessions_init(rbtree_t* tree);

extern void dtls_server_flush_write_bio(xylem_dtls_conn_t* dtls);
extern int dtls_server_send_record(xylem_dtls_conn_t* dtls,
                                   const void* data, int len);

extern int dtls_init_ssl(xylem_dtls_conn_t* dtls, SSL_CTX* ssl_ctx);

extern int dtls_handle_io_block(xylem_dtls_conn_t* dtls, int ssl_err,
                                const char* op_name);

extern int dtls_client_do_handshake(xylem_dtls_conn_t* dtls,
                                    uint64_t deadline);

extern void dtls_cache_alpn(xylem_dtls_conn_t* dtls);

extern void dtls_arm_retransmit(xylem_dtls_conn_t* dtls);
extern void dtls_stop_retransmit(xylem_dtls_conn_t* dtls);
