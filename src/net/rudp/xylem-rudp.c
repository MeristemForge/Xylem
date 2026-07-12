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

#include "xylem/net/xylem-rudp.h"
#include "xylem/crypto/xylem-aes256.h"
#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "container/queue.h"
#include "container/rbtree.h"
#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "xylem/xylem-threads.h"

#include "rudp-fec.h"
#include "kcp/ikcp.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUDP_DEFAULT_MTU         1400
#define RUDP_DEFAULT_TIMEOUT_MS  5000
#define RUDP_DEFAULT_DEADLINK_MS 30000
#define RUDP_RECV_BUF_SIZE       1500
#define RUDP_HANDSHAKE_MAGIC     0x58594C4D /* "XYLM" */
#define RUDP_HANDSHAKE_SYN       0x01
#define RUDP_HANDSHAKE_ACK       0x02
#define RUDP_HANDSHAKE_SIZE      9
#define RUDP_SYN_RETRANSMIT_MS   1000
#define RUDP_AES_IV_SIZE         16
#define RUDP_INBOX_CAP           64
#define RUDP_ACCEPT_CAP          16
#define RUDP_SERVER_WRITE_POLL_MS 50

typedef struct _rudp_dgram_s {
    size_t len;
    char   data[];
} _rudp_dgram_t;

typedef struct _rudp_out_s {
    queue_node_t node;
    size_t       len;
    char         data[];
} _rudp_out_t;

typedef enum {
    RUDP_SEND_ERROR = -1,
    RUDP_SEND_OK    = 0,
    RUDP_SEND_AGAIN = 1,
} _rudp_send_result_t;

struct xylem_rudp_conn_s {
    ikcpcb*                kcp;
    platform_sock_t        fd;
    iowait_t*              waiter;
    xylem_rudp_mode_t      mode;
    addr_t                 peer_addr;
    _Atomic bool           closed;
    _Atomic int32_t        refcnt;

    scheduler_timer_t*     update_timer;
    xylem_mutex_t*         kcp_mu;      /* serializes KCP/FEC state. */
    xylem_mutex_t*         rd_mu;       /* sole reader/parker. */
    xylem_mutex_t*         wr_mu;       /* sole writer/parker. */

    rudp_fec_enc_t*        fec_enc;
    rudp_fec_dec_t*        fec_dec;
    xylem_aes256_t*        aes;
    queue_t                pending_out;

    uint32_t               conv;
    int                    mtu;
    _Atomic uint64_t       rd_deadline_ms;
    _Atomic uint64_t       wr_deadline_ms;

    xylem_rudp_listener_t* listener;
    xylem_channel_t*       inbox;        /* server session datagram queue */
    _Atomic int32_t        inbox_len;    /* bounded-queue guard */
    rbtree_node_t          listener_node;
    bool                   in_listener;
    bool                   accepted;
};

struct xylem_rudp_listener_s {
    platform_sock_t        fd;
    iowait_t*              waiter;
    xylem_rudp_opts_t      opts;
    scheduler_t*           sched;
    rbtree_t               sessions;
    mtx_t                  sessions_mtx;
    xylem_mutex_t*         write_mu;
    xylem_aes256_t*        aes;
    uint8_t                aes_key_buf[32];
    _Atomic bool           closed;
    _Atomic int32_t        refcnt;

    xylem_channel_t*       accept_ch;   /* delivers accepted sessions */
};

typedef struct _rudp_session_key_s {
    const addr_t* addr;
    uint32_t      conv;
} _rudp_session_key_t;

static _Atomic uint32_t _rudp_next_conv = 0;

static uint32_t _rudp_clock_ms(void) {
    return (uint32_t)(xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) &
                      0xFFFFFFFF);
}

static uint32_t _rudp_alloc_conv(void) {
    uint32_t v = atomic_load(&_rudp_next_conv);
    if (v == 0) {
        uint32_t seed = (uint32_t)xylem_utils_getprng(1, 0x7FFFFFFF);
        atomic_compare_exchange_strong(&_rudp_next_conv, &v, seed);
    }
    return atomic_fetch_add(&_rudp_next_conv, 1);
}

static void _rudp_encode_handshake(uint8_t* buf, uint8_t type,
                                   uint32_t conv) {
    uint32_t magic = RUDP_HANDSHAKE_MAGIC;
    memcpy(buf, &magic, 4);
    buf[4] = type;
    memcpy(buf + 5, &conv, 4);
}

static int _rudp_decode_handshake(const void* data, size_t len,
                                  uint8_t* type, uint32_t* conv) {
    if (len != RUDP_HANDSHAKE_SIZE) {
        return -1;
    }
    const uint8_t* buf = (const uint8_t*)data;
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != RUDP_HANDSHAKE_MAGIC) {
        return -1;
    }
    *type = buf[4];
    memcpy(conv, buf + 5, 4);
    return 0;
}

/**
 * Send raw bytes on a client connection (connected socket) or
 * a server session (sendto peer address).
 */
static bool _rudp_is_again(int err) {
    return err == PLATFORM_SO_ERROR_EAGAIN
           || err == PLATFORM_SO_ERROR_EWOULDBLOCK;
}

static _rudp_send_result_t _rudp_raw_send(
    xylem_rudp_conn_t* c,
    const void*        data,
    size_t             len) {
    ssize_t n;
    if (c->listener) {
        /* Server session: unconnected socket, use sendto. */
        socklen_t addrlen =
            (c->peer_addr.storage.ss_family == AF_INET6)
                ? (socklen_t)sizeof(struct sockaddr_in6)
                : (socklen_t)sizeof(struct sockaddr_in);
        n = platform_socket_sendto(
            c->fd, data, (int)len,
            &c->peer_addr.storage, addrlen);
    } else {
        /* Client: connected socket. */
        n = platform_socket_send(c->fd, data, (int)len);
    }

    if (n == (ssize_t)len) {
        return RUDP_SEND_OK;
    }
    if (n < 0 && _rudp_is_again(platform_socket_get_lasterror())) {
        return RUDP_SEND_AGAIN;
    }
    return RUDP_SEND_ERROR;
}

static void _rudp_pending_out_free(xylem_rudp_conn_t* c) {
    queue_node_t* node;
    while ((node = queue_dequeue(&c->pending_out)) != NULL) {
        _rudp_out_t* out = queue_entry(node, _rudp_out_t, node);
        free(out);
    }
}

static int _rudp_pending_out_push(
    xylem_rudp_conn_t* c,
    const void*        data,
    size_t             len) {
    _rudp_out_t* out = (_rudp_out_t*)malloc(sizeof(_rudp_out_t) + len);
    if (!out) {
        return -1;
    }
    out->len = len;
    memcpy(out->data, data, len);

    queue_enqueue(&c->pending_out, &out->node);
    return 0;
}

static _rudp_send_result_t _rudp_pending_out_drain_locked(
    xylem_rudp_conn_t* c) {
    while (!queue_empty(&c->pending_out)) {
        queue_node_t* node = queue_front(&c->pending_out);
        _rudp_out_t* out = queue_entry(node, _rudp_out_t, node);
        _rudp_send_result_t rc = _rudp_raw_send(c, out->data, out->len);
        if (rc == RUDP_SEND_AGAIN) {
            return rc;
        }
        (void)queue_dequeue(&c->pending_out);
        free(out);
        if (rc != RUDP_SEND_OK) {
            return rc;
        }
    }
    return RUDP_SEND_OK;
}

/**
 * AES encrypt then non-blocking send. If the socket cannot accept the
 * datagram, queue the fully encoded datagram for the outer write wait path.
 */
static int _rudp_encrypt_send(
    xylem_rudp_conn_t* c,
    const void*        data,
    size_t             len) {
    const void* send_data = data;
    size_t      send_len  = len;
    uint8_t*    enc_buf   = NULL;

    if (c->aes) {
        size_t enc_size = xylem_aes256_ctr_encrypt_size(len);
        enc_buf = (uint8_t*)malloc(enc_size);
        if (!enc_buf) {
            return -1;
        }
        int n = xylem_aes256_ctr_encrypt(
            c->aes, (const uint8_t*)data, len, enc_buf, enc_size);
        if (n <= 0) {
            free(enc_buf);
            return -1;
        }
        send_data = enc_buf;
        send_len  = (size_t)n;
    }

    int ret = 0;
    if (queue_empty(&c->pending_out)) {
        _rudp_send_result_t rc = _rudp_raw_send(c, send_data, send_len);
        if (rc == RUDP_SEND_AGAIN) {
            ret = _rudp_pending_out_push(c, send_data, send_len);
        } else if (rc != RUDP_SEND_OK) {
            ret = -1;
        }
    } else {
        ret = _rudp_pending_out_push(c, send_data, send_len);
    }

    free(enc_buf);
    return ret;
}

/**
 * AES decrypt in-place or allocate. When AES is NULL, returns
 * the original pointer without allocation.
 * Returns 0 on success, -1 on failure.
 */
static int _rudp_decrypt_packet(xylem_aes256_t* aes, void* data,
                                size_t len, void** out, size_t* out_len) {
    if (!aes) {
        *out     = data;
        *out_len = len;
        return 0;
    }
    size_t dec_size = xylem_aes256_ctr_decrypt_size(len);
    if (dec_size == 0) {
        return -1;
    }
    uint8_t* dec_buf = (uint8_t*)malloc(dec_size);
    if (!dec_buf) {
        return -1;
    }
    int n = xylem_aes256_ctr_decrypt(
        aes, (const uint8_t*)data, len, dec_buf, dec_size);
    if (n <= 0) {
        free(dec_buf);
        return -1;
    }
    *out     = dec_buf;
    *out_len = (size_t)n;
    return 0;
}

static int _rudp_session_cmp_nn(const rbtree_node_t* a,
                                const rbtree_node_t* b) {
    const xylem_rudp_conn_t* ca =
        rbtree_entry(a, xylem_rudp_conn_t, listener_node);
    const xylem_rudp_conn_t* cb =
        rbtree_entry(b, xylem_rudp_conn_t, listener_node);

    int rc = memcmp(&ca->peer_addr.storage, &cb->peer_addr.storage,
                    sizeof(struct sockaddr_storage));
    if (rc != 0) {
        return rc;
    }
    if (ca->conv < cb->conv) {
        return -1;
    }
    if (ca->conv > cb->conv) {
        return 1;
    }
    return 0;
}

static int _rudp_session_cmp_kn(const void* key,
                                const rbtree_node_t* node) {
    const _rudp_session_key_t* k = (const _rudp_session_key_t*)key;
    const xylem_rudp_conn_t* c =
        rbtree_entry(node, xylem_rudp_conn_t, listener_node);

    int rc = memcmp(&k->addr->storage, &c->peer_addr.storage,
                    sizeof(struct sockaddr_storage));
    if (rc != 0) {
        return rc;
    }
    if (k->conv < c->conv) {
        return -1;
    }
    if (k->conv > c->conv) {
        return 1;
    }
    return 0;
}

static xylem_rudp_conn_t* _rudp_find_session(
    xylem_rudp_listener_t* ln, const addr_t* addr, uint32_t conv) {
    _rudp_session_key_t key = { .addr = addr, .conv = conv };
    rbtree_node_t* node = rbtree_find(&ln->sessions, &key);
    if (!node) {
        return NULL;
    }
    return rbtree_entry(node, xylem_rudp_conn_t, listener_node);
}

static int _rudp_kcp_output_cb(const char* buf, int len,
                               ikcpcb* kcp, void* user) {
    (void)kcp;
    xylem_rudp_conn_t* c = (xylem_rudp_conn_t*)user;
    if (atomic_load(&c->closed)) {
        return -1;
    }

    if (c->fec_enc) {
        /* Feed each KCP segment through FEC; send resulting shards. */
        int max_out = rudp_fec_enc_feed_size(c->fec_enc);
        rudp_fec_buf_t shards[RUDP_FEC_MAX_SHARDS];
        int n = rudp_fec_enc_feed(
            c->fec_enc, buf, (size_t)len, shards, max_out);
        for (int i = 0; i < n; i++) {
            if (_rudp_encrypt_send(c, shards[i].data, shards[i].len) != 0) {
                return -1;
            }
        }
    } else {
        if (_rudp_encrypt_send(c, buf, (size_t)len) != 0) {
            return -1;
        }
    }
    return 0;
}

static int _rudp_conn_init_mutexes(xylem_rudp_conn_t* c) {
    c->kcp_mu = xylem_mutex_create();
    c->rd_mu  = xylem_mutex_create();
    c->wr_mu  = xylem_mutex_create();
    if (!c->kcp_mu || !c->rd_mu || !c->wr_mu) {
        xylem_mutex_destroy(c->kcp_mu);
        xylem_mutex_destroy(c->rd_mu);
        xylem_mutex_destroy(c->wr_mu);
        c->kcp_mu = NULL;
        c->rd_mu  = NULL;
        c->wr_mu  = NULL;
        return -1;
    }
    return 0;
}

static void _rudp_conn_destroy_mutexes(xylem_rudp_conn_t* c) {
    xylem_mutex_destroy(c->kcp_mu);
    xylem_mutex_destroy(c->rd_mu);
    xylem_mutex_destroy(c->wr_mu);
    c->kcp_mu = NULL;
    c->rd_mu  = NULL;
    c->wr_mu  = NULL;
}

static iowait_t* _rudp_write_waiter(xylem_rudp_conn_t* c) {
    return c->listener ? c->listener->waiter : c->waiter;
}

static int _rudp_wait_write(xylem_rudp_conn_t* c) {
    iowait_t* waiter = _rudp_write_waiter(c);
    if (!waiter) {
        return -1;
    }

    xylem_mutex_t* fd_mu = c->listener ? c->listener->write_mu : NULL;
    if (fd_mu) {
        xylem_mutex_lock(fd_mu);
    }

    iowait_result_t r = IOWAIT_ERROR;
    for (;;) {
        uint64_t deadline = atomic_load(&c->wr_deadline_ms);
        if (c->listener) {
            uint64_t now =
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (deadline > 0 && now >= deadline) {
                r = IOWAIT_TIMEOUT;
                break;
            }
            uint64_t poll_deadline = now + RUDP_SERVER_WRITE_POLL_MS;
            if (deadline == 0 || poll_deadline < deadline) {
                deadline = poll_deadline;
            }
        }

        if (deadline > 0) {
            iowait_set_wr_deadline(waiter, deadline);
        }
        r = iowait_write(waiter);
        if (c->listener && deadline > 0) {
            iowait_set_wr_deadline(waiter, 0);
        }
        if (!c->listener || r != IOWAIT_TIMEOUT
            || atomic_load(&c->closed)) {
            break;
        }
    }

    if (fd_mu) {
        xylem_mutex_unlock(fd_mu);
    }

    if (r != IOWAIT_READY || atomic_load(&c->closed)) {
        return -1;
    }
    return 0;
}

static int _rudp_drain_pending(xylem_rudp_conn_t* c, bool owns_wr_mu) {
    if (atomic_load(&c->closed)) {
        return -1;
    }
    if (!owns_wr_mu) {
        xylem_mutex_lock(c->wr_mu);
    }

    int ret = -1;
    for (;;) {
        xylem_mutex_lock(c->kcp_mu);
        _rudp_send_result_t rc = _rudp_pending_out_drain_locked(c);
        xylem_mutex_unlock(c->kcp_mu);

        if (rc == RUDP_SEND_OK) {
            ret = 0;
            break;
        }
        if (rc != RUDP_SEND_AGAIN || atomic_load(&c->closed)) {
            break;
        }
        if (_rudp_wait_write(c) != 0) {
            break;
        }
    }

    if (!owns_wr_mu) {
        xylem_mutex_unlock(c->wr_mu);
    }
    return ret;
}

static uint64_t _rudp_kcp_update_delay_locked(xylem_rudp_conn_t* c) {
    uint32_t now  = _rudp_clock_ms();
    uint32_t next = ikcp_check(c->kcp, now);
    return (next <= now) ? 1 : (uint64_t)(next - now);
}

static int _rudp_kcp_recv(
    xylem_rudp_conn_t* c,
    void*              buf,
    int                len) {
    xylem_mutex_lock(c->kcp_mu);
    int n = ikcp_recv(c->kcp, (char*)buf, len);
    xylem_mutex_unlock(c->kcp_mu);
    return n;
}

static void _rudp_schedule_update(xylem_rudp_conn_t* c) {
    if (atomic_load(&c->closed) || !c->update_timer) {
        return;
    }

    xylem_mutex_lock(c->kcp_mu);
    uint64_t delay = _rudp_kcp_update_delay_locked(c);
    xylem_mutex_unlock(c->kcp_mu);
    scheduler_timer_reset(c->update_timer, delay);
}

static ikcpcb* _rudp_create_kcp(xylem_rudp_conn_t* c, uint32_t conv,
                                xylem_rudp_opts_t* opts) {
    ikcpcb* kcp = ikcp_create(conv, c);
    if (!kcp) {
        return NULL;
    }
    ikcp_setoutput(kcp, _rudp_kcp_output_cb);

    /* Fast mode: nodelay=1, interval=10ms, fast-resend=2, no flow ctrl. */
    ikcp_nodelay(kcp, 1, 10, 2, 1);
    ikcp_wndsize(kcp, 32, 128);

    if (opts && opts->mode == XYLEM_RUDP_STREAM) {
        kcp->stream = 1;
    }

    int mtu = RUDP_DEFAULT_MTU;
    if (opts && opts->mtu > 0) {
        mtu = (int)opts->mtu;
    }
    /* Reserve space for FEC header and AES IV. */
    if (opts && opts->fec_data > 0 && opts->fec_parity > 0) {
        mtu -= RUDP_FEC_HEADER_SIZE;
    }
    if (opts && opts->aes_key) {
        mtu -= RUDP_AES_IV_SIZE;
    }
    ikcp_setmtu(kcp, mtu);

    uint64_t timeout_ms = RUDP_DEFAULT_DEADLINK_MS;
    if (opts && opts->timeout_ms > 0) {
        timeout_ms = opts->timeout_ms;
    }
    /* dead_link is checked each interval (10ms). */
    kcp->dead_link = (IUINT32)(timeout_ms / 10);
    if (kcp->dead_link == 0) {
        kcp->dead_link = 1;
    }

    return kcp;
}

static int _rudp_init_fec(xylem_rudp_conn_t* c, int mtu,
                          uint32_t fec_data, uint32_t fec_parity) {
    if (fec_data == 0 || fec_parity == 0) {
        return 0;
    }
    c->fec_enc = rudp_fec_enc_create(
        (int)fec_data, (int)fec_parity, mtu);
    if (!c->fec_enc) {
        return -1;
    }
    c->fec_dec = rudp_fec_dec_create(
        (int)fec_data, (int)fec_parity, mtu);
    if (!c->fec_dec) {
        rudp_fec_enc_destroy(c->fec_enc);
        c->fec_enc = NULL;
        return -1;
    }
    return 0;
}

/**
 * Feed a decrypted packet into FEC decoder (if enabled) then into
 * KCP. Always flushes KCP after input so ACKs go out immediately.
 */
static int _rudp_recv_input_locked(
    xylem_rudp_conn_t* c,
    void*              data,
    size_t             len) {
    if (!c->fec_dec) {
        ikcp_input(c->kcp, (const char*)data, (long)len);
        ikcp_flush(c->kcp);
        return 0;
    }

    int max_out = rudp_fec_dec_feed_size(c->fec_dec);
    rudp_fec_buf_t out[RUDP_FEC_MAX_SHARDS];
    int n = rudp_fec_dec_feed(c->fec_dec, data, len, out, max_out);
    if (n < 0) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        ikcp_input(c->kcp, (const char*)out[i].data, (long)out[i].len);
    }
    if (n > 0) {
        ikcp_flush(c->kcp);
    }
    return 0;
}

static int _rudp_recv_input(
    xylem_rudp_conn_t* c,
    void*              data,
    size_t             len) {
    xylem_mutex_lock(c->kcp_mu);
    int ret = _rudp_recv_input_locked(c, data, len);
    xylem_mutex_unlock(c->kcp_mu);
    if (ret != 0) {
        return ret;
    }
    return _rudp_drain_pending(c, false);
}

static int _rudp_decrypt_recv_input(
    xylem_rudp_conn_t* c,
    void*              data,
    size_t             len) {
    xylem_mutex_lock(c->kcp_mu);
    void*  plain     = NULL;
    size_t plain_len = 0;
    int ret = _rudp_decrypt_packet(c->aes, data, len, &plain, &plain_len);
    if (ret == 0) {
        ret = _rudp_recv_input_locked(c, plain, plain_len);
    }
    if (plain != data) {
        free(plain);
    }
    xylem_mutex_unlock(c->kcp_mu);
    if (ret != 0) {
        return ret;
    }
    return _rudp_drain_pending(c, false);
}

static void _rudp_conn_ref(void* ud);
static void _rudp_conn_unref(void* ud);
static void _rudp_deferred_close(void* arg);
static void _rudp_conn_shutdown(xylem_rudp_conn_t* conn);

static void _rudp_listener_ref(xylem_rudp_listener_t* ln) {
    atomic_fetch_add(&ln->refcnt, 1);
}

static void _rudp_accept_ch_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }
    xylem_rudp_conn_t* conn;
    while ((conn = (xylem_rudp_conn_t*)xylem_channel_recv_timeout(ch, 0))
           != NULL) {
        _rudp_conn_unref(conn);
    }
    xylem_channel_destroy(ch);
}

static void _rudp_listener_unref(xylem_rudp_listener_t* ln) {
    if (atomic_fetch_sub(&ln->refcnt, 1)
        != 1) {
        return;
    }

    if (ln->accept_ch) {
        _rudp_accept_ch_destroy(ln->accept_ch);
    }
    xylem_mutex_destroy(ln->write_mu);
    xylem_aes256_destroy(ln->aes);
    memset(ln->aes_key_buf, 0, sizeof(ln->aes_key_buf));
    if (ln->waiter) {
        iowait_destroy(ln->waiter);
    }
    if (ln->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(ln->fd);
    }
    mtx_destroy(&ln->sessions_mtx);
    free(ln);
}

static void _rudp_update_timer_cb(scheduler_timer_t* timer, void* ud) {
    (void)timer;
    xylem_rudp_conn_t* c = (xylem_rudp_conn_t*)ud;
    if (atomic_load(&c->closed)) {
        return;
    }

    uint32_t now = _rudp_clock_ms();
    xylem_mutex_lock(c->kcp_mu);
    ikcp_update(c->kcp, now);
    bool dead = c->kcp->state == (IUINT32)-1;
    xylem_mutex_unlock(c->kcp_mu);
    (void)_rudp_drain_pending(c, false);

    /* Detect dead link. */
    if (dead) {
        xylem_loge("<rudp> dead link conv=%u", c->conv);
        /**
         * The update timer callback itself is spawned. Defer teardown one
         * more scheduler turn so this timer fire can return and release its
         * ud guard before close/shutdown drops owner references or destroys
         * timer-owned resources. The extra reference keeps the conn alive
         * until the deferred coroutine runs.
         */
        _rudp_conn_ref(c);
        if (runtime_spawn(_rudp_deferred_close, c) != 0) {
            _rudp_conn_unref(c);
        }
        return;
    }

    _rudp_schedule_update(c);
}

/**
 * Drain residual datagrams from an already-closed session inbox and
 * destroy it. The caller (xylem_rudp_close) has already closed the
 * channel, so the drain recv() never parks: it pops any payloads the
 * dispatcher queued, then returns NULL once empty. We free the
 * payloads here because xylem_channel_destroy only frees the node
 * wrappers, not the opaque dgram pointers. Runs from _rudp_conn_unref
 * at refcount zero, so no other thread touches the channel.
 */
static void _rudp_inbox_destroy(xylem_channel_t* ch) {
    if (!ch) {
        return;
    }
    _rudp_dgram_t* dgram;
    while ((dgram = (_rudp_dgram_t*)xylem_channel_recv(ch)) != NULL) {
        free(dgram);
    }
    xylem_channel_destroy(ch);
}

/**
 * Copy a datagram into the session inbox channel. The reader frees it.
 * Bounded by RUDP_INBOX_CAP via conn->inbox_len: when the queue is
 * full the datagram is dropped (KCP retransmits), preserving the
 * back-pressure the old ring buffer provided over an unbounded channel.
 * The dispatcher holds sessions_mtx across find+push so the session
 * cannot be freed under us.
 */
static void _rudp_inbox_push(xylem_rudp_conn_t* sess, const void* data,
                             size_t len) {
    if (atomic_load(&sess->inbox_len)
        >= (int32_t)RUDP_INBOX_CAP) {
        return; /* queue full: drop, KCP will retransmit */
    }

    _rudp_dgram_t* dgram =
        (_rudp_dgram_t*)malloc(sizeof(_rudp_dgram_t) + len);
    if (!dgram) {
        return;
    }
    dgram->len = len;
    memcpy(dgram->data, data, len);

    atomic_fetch_add(&sess->inbox_len, 1);
    if (xylem_channel_send(sess->inbox, dgram) != 0) {
        atomic_fetch_sub(&sess->inbox_len, 1);
        free(dgram);
    }
}

static void _rudp_conn_ref(void* ud) {
    xylem_rudp_conn_t* conn = (xylem_rudp_conn_t*)ud;
    atomic_fetch_add(&conn->refcnt, 1);
}

/**
 * Drop a reference; the last one out performs the actual teardown.
 *
 * A reader parked in xylem_channel_recv / iowait_read holds a conn
 * reference across the park, so a concurrent xylem_rudp_close only marks
 * the session closed and wakes the reader. The inbox and KCP state stay
 * alive until the woken reader drops its reference here.
 */
static void _rudp_conn_unref(void* ud) {
    xylem_rudp_conn_t* conn = (xylem_rudp_conn_t*)ud;
    if (atomic_fetch_sub(&conn->refcnt, 1)
        != 1) {
        return;
    }

    if (conn->update_timer) {
        scheduler_timer_destroy(conn->update_timer);
        conn->update_timer = NULL;
    }

    if (conn->kcp) {
        ikcp_release(conn->kcp);
        conn->kcp = NULL;
    }

    _rudp_pending_out_free(conn);
    rudp_fec_enc_destroy(conn->fec_enc);
    rudp_fec_dec_destroy(conn->fec_dec);
    conn->fec_enc = NULL;
    conn->fec_dec = NULL;

    if (!conn->listener) {
        /* Client owns its socket, waiter and AES context. */
        /**
         * Disarm any in-flight deadline timer before teardown. iowait
         * close/destroy do not cancel timers, and an armed timer holds
         * an iowait reference -- without this the waiter (slab slot)
         * would linger until a stale deadline set by the caller fires.
         */
        if (conn->waiter) {
            iowait_set_rd_deadline(conn->waiter, 0);
            iowait_set_wr_deadline(conn->waiter, 0);
        }
        iowait_destroy(conn->waiter);
        platform_socket_close(conn->fd);
        xylem_aes256_destroy(conn->aes);
    } else {
        xylem_rudp_listener_t* ln = conn->listener;
        bool accepted = conn->accepted;
        _rudp_inbox_destroy(conn->inbox);
        conn->inbox = NULL;
        xylem_aes256_destroy(conn->aes);
        if (accepted) {
            _rudp_listener_unref(ln);
        }
    }

    _rudp_conn_destroy_mutexes(conn);
    free(conn);
}

/**
 * Coroutine entry that performs a deferred dead-link teardown. Used by
 * the update timer's dead-link path to keep teardown out of the timer
 * callback frame. Drops the reference taken when the teardown was scheduled.
 *
 * The timer must only perform shutdown. Application/accept references
 * remain owned by their callers and are dropped by xylem_rudp_close().
 * For listener sessions, shutdown removes the session tree reference.
 */
static void _rudp_deferred_close(void* arg) {
    xylem_rudp_conn_t* conn = (xylem_rudp_conn_t*)arg;
    _rudp_conn_shutdown(conn); /* wake readers; owner refs close normally */
    _rudp_conn_unref(conn); /* drop the ref taken by the timer callback */
}

static int _rudp_client_read(xylem_rudp_conn_t* c, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    char recv_buf[RUDP_RECV_BUF_SIZE];
    for (;;) {
        int n = _rudp_kcp_recv(c, buf, len);
        if (n > 0) {
            return n;
        }

        if (atomic_load(&c->closed)) {
            return -1;
        }

        uint64_t rd_deadline = atomic_load(&c->rd_deadline_ms);
        if (rd_deadline > 0) {
            iowait_set_rd_deadline(c->waiter, rd_deadline);
        }

        iowait_result_t r = iowait_read(c->waiter);
        if (r == IOWAIT_CLOSED) {
            return -1;
        }
        if (r == IOWAIT_TIMEOUT) {
            xylem_mutex_lock(c->kcp_mu);
            ikcp_update(c->kcp, _rudp_clock_ms());
            xylem_mutex_unlock(c->kcp_mu);
            (void)_rudp_drain_pending(c, false);
            _rudp_schedule_update(c);
            return -1;
        }
        if (r != IOWAIT_READY) {
            return -1;
        }

        /* ET poller: drain all queued datagrams until EAGAIN. */
        for (;;) {
            ssize_t rn = platform_socket_recv(
                c->fd, recv_buf, (int)sizeof(recv_buf));
            if (rn <= 0) {
                break;
            }

            (void)_rudp_decrypt_recv_input(c, recv_buf, (size_t)rn);
        }

        _rudp_schedule_update(c);
    }
}

static int _rudp_session_read(xylem_rudp_conn_t* c, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    for (;;) {
        int n = _rudp_kcp_recv(c, buf, len);
        if (n > 0) {
            return n;
        }

        if (atomic_load(&c->closed)) {
            return -1;
        }

        /**
         * Pull a datagram from the inbox channel (parks if empty;
         * returns NULL once the channel is closed on teardown).
         */
        _rudp_dgram_t* dgram =
            (_rudp_dgram_t*)xylem_channel_recv(c->inbox);
        if (!dgram) {
            return -1;
        }
        atomic_fetch_sub(&c->inbox_len, 1);

        (void)_rudp_recv_input(c, dgram->data, dgram->len);
        free(dgram);
        _rudp_schedule_update(c);
    }
}

/**
 * Build a server session for a freshly handshaked peer and publish it:
 * insert into the session tree, start its update timer and hand it to
 * the accept channel. Returns 0 on success, -1 if any resource could
 * not be allocated (all partially built state is freed before return).
 */
static int _rudp_accept_session(xylem_rudp_listener_t* ln,
                                const addr_t* peer_addr, uint32_t hs_conv) {
    xylem_rudp_conn_t* sess =
        (xylem_rudp_conn_t*)calloc(1, sizeof(xylem_rudp_conn_t));
    if (!sess) {
        return -1;
    }

    atomic_store(&sess->refcnt, 1);
    queue_init(&sess->pending_out);
    sess->fd        = ln->fd;
    sess->conv      = hs_conv;
    sess->peer_addr = *peer_addr;
    sess->listener  = ln;
    sess->mode      = ln->opts.mode;
    sess->mtu       = (ln->opts.mtu > 0)
        ? (int)ln->opts.mtu : RUDP_DEFAULT_MTU;

    if (_rudp_init_fec(sess, sess->mtu, ln->opts.fec_data,
                       ln->opts.fec_parity) != 0) {
        free(sess);
        return -1;
    }

    if (_rudp_conn_init_mutexes(sess) != 0) {
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    if (ln->opts.aes_key) {
        sess->aes = xylem_aes256_create(ln->opts.aes_key);
        if (!sess->aes) {
            _rudp_conn_destroy_mutexes(sess);
            rudp_fec_enc_destroy(sess->fec_enc);
            rudp_fec_dec_destroy(sess->fec_dec);
            free(sess);
            return -1;
        }
    }

    sess->kcp = _rudp_create_kcp(sess, hs_conv, &ln->opts);
    if (!sess->kcp) {
        xylem_aes256_destroy(sess->aes);
        _rudp_conn_destroy_mutexes(sess);
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    sess->inbox = xylem_channel_create();
    if (!sess->inbox) {
        ikcp_release(sess->kcp);
        xylem_aes256_destroy(sess->aes);
        _rudp_conn_destroy_mutexes(sess);
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    sess->update_timer = scheduler_timer_create(ln->sched);
    if (!sess->update_timer) {
        xylem_channel_destroy(sess->inbox);
        ikcp_release(sess->kcp);
        xylem_aes256_destroy(sess->aes);
        _rudp_conn_destroy_mutexes(sess);
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    scheduler_timer_set_ud_guard(
        sess->update_timer, _rudp_conn_ref, _rudp_conn_unref);
    scheduler_timer_set_spawn(sess->update_timer, true);
    scheduler_timer_start(
            sess->update_timer, _rudp_update_timer_cb, sess, 10, 0);
    _rudp_schedule_update(sess);

    bool published = false;
    mtx_lock(&ln->sessions_mtx);
    if (!atomic_load(&ln->closed)) {
        rbtree_insert(&ln->sessions, &sess->listener_node);
        sess->in_listener = true;
        _rudp_conn_ref(sess); /* accept/user reference */
        if (xylem_channel_send(ln->accept_ch, sess) == 0) {
            published = true;
        } else {
            rbtree_remove(&ln->sessions, &sess->listener_node);
            sess->in_listener = false;
            _rudp_conn_unref(sess);
        }
    }
    mtx_unlock(&ln->sessions_mtx);

    if (!published) {
        _rudp_conn_shutdown(sess);
        _rudp_conn_unref(sess);
        return -1;
    }

    return 0;
}

static void _rudp_dispatcher(void* arg) {
    xylem_rudp_listener_t* ln = (xylem_rudp_listener_t*)arg;
    char recv_buf[RUDP_RECV_BUF_SIZE];

    for (;;) {
        if (atomic_load(&ln->closed)) {
            break;
        }

        struct sockaddr_storage sender;
        socklen_t sender_len = sizeof(sender);
        ssize_t n = platform_socket_recvfrom(
            ln->fd, recv_buf, (int)sizeof(recv_buf),
            &sender, &sender_len);

        if (n <= 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                iowait_result_t r = iowait_read(ln->waiter);
                if (r != IOWAIT_READY) {
                    break;
                }
                continue;
            }
            /* Fatal recv error. */
            xylem_loge("rudp listener: recvfrom error=%d (%s)",
                       err, platform_socket_tostring(err));
            break;
        }

        /* Decrypt outer layer. */
        void*  plain     = NULL;
        size_t plain_len = 0;
        if (_rudp_decrypt_packet(ln->aes, recv_buf, (size_t)n,
                                 &plain, &plain_len) != 0) {
            continue;
        }

        addr_t peer_addr;
        memcpy(&peer_addr.storage, &sender, sizeof(sender));

        /* Check if this is a handshake packet. */
        uint8_t  hs_type;
        uint32_t hs_conv;
        if (_rudp_decode_handshake(plain, plain_len,
                                   &hs_type, &hs_conv) == 0) {
            if (hs_type == RUDP_HANDSHAKE_SYN) {
                /* Send ACK regardless (handles retransmitted SYNs). */
                uint8_t ack[RUDP_HANDSHAKE_SIZE];
                _rudp_encode_handshake(ack, RUDP_HANDSHAKE_ACK, hs_conv);

                /* Encrypt and sendto. */
                if (ln->aes) {
                    size_t enc_size =
                        xylem_aes256_ctr_encrypt_size(RUDP_HANDSHAKE_SIZE);
                    uint8_t* enc_buf = (uint8_t*)malloc(enc_size);
                    if (enc_buf) {
                        int en = xylem_aes256_ctr_encrypt(
                            ln->aes, ack, RUDP_HANDSHAKE_SIZE,
                            enc_buf, enc_size);
                        if (en > 0) {
                            socklen_t addrlen =
                                (sender.ss_family == AF_INET6)
                                    ? (socklen_t)sizeof(struct sockaddr_in6)
                                    : (socklen_t)sizeof(struct sockaddr_in);
                            platform_socket_sendto(
                                ln->fd, enc_buf, en, &sender, addrlen);
                        }
                        free(enc_buf);
                    }
                } else {
                    socklen_t addrlen =
                        (sender.ss_family == AF_INET6)
                            ? (socklen_t)sizeof(struct sockaddr_in6)
                            : (socklen_t)sizeof(struct sockaddr_in);
                    platform_socket_sendto(
                        ln->fd, ack, RUDP_HANDSHAKE_SIZE,
                        &sender, addrlen);
                }

                /* Create session if it does not exist. */
                mtx_lock(&ln->sessions_mtx);
                bool exists = _rudp_find_session(ln, &peer_addr, hs_conv)
                              != NULL;
                mtx_unlock(&ln->sessions_mtx);
                if (!exists) {
                    _rudp_accept_session(ln, &peer_addr, hs_conv);
                }
            }
            /* Ignore ACKs on server side. */
        } else {
            /* Data packet: route to the correct session. */
            if (ln->opts.fec_data > 0 && ln->opts.fec_parity > 0 &&
                plain_len >= RUDP_FEC_HEADER_SIZE) {
                /**
                 * FEC-encoded: check shard type. Data shards carry
                 * the KCP header at offset 8, so conv is extractable.
                 * Parity shards have no KCP payload; route to all
                 * sessions from this peer (typically one).
                 */
                const uint8_t* p = (const uint8_t*)plain;
                uint16_t fec_type = (uint16_t)(
                    (uint16_t)p[4] | ((uint16_t)p[5] << 8));

                if (fec_type == RUDP_FEC_TYPE_DATA &&
                    plain_len >= RUDP_FEC_HEADER_SIZE + 4) {
                    uint32_t conv;
                    memcpy(&conv, p + RUDP_FEC_HEADER_SIZE, 4);
                    /**
                     * Hold the lock across find+push so the session
                     * cannot be removed and freed between lookup and
                     * inbox push. _rudp_inbox_push only enqueues (it
                     * never parks), so this critical section is short.
                     */
                    mtx_lock(&ln->sessions_mtx);
                    xylem_rudp_conn_t* sess =
                        _rudp_find_session(ln, &peer_addr, conv);
                    if (sess && sess->inbox) {
                        _rudp_inbox_push(sess, plain, plain_len);
                    }
                    mtx_unlock(&ln->sessions_mtx);
                } else if (fec_type == RUDP_FEC_TYPE_PARITY) {
                    /**
                     * Parity shards cannot be keyed by conv; deliver
                     * to every session from this peer so each FEC
                     * decoder can attempt recovery.
                     */
                    mtx_lock(&ln->sessions_mtx);
                    rbtree_node_t* nd = rbtree_min(&ln->sessions);
                    while (nd) {
                        xylem_rudp_conn_t* s = rbtree_entry(
                            nd, xylem_rudp_conn_t, listener_node);
                        nd = rbtree_next(nd);
                        if (memcmp(&s->peer_addr.storage,
                                   &peer_addr.storage,
                                   sizeof(struct sockaddr_storage))
                            == 0 && s->inbox) {
                            _rudp_inbox_push(s, plain, plain_len);
                        }
                    }
                    mtx_unlock(&ln->sessions_mtx);
                }
            } else if (plain_len >= 4) {
                /* No FEC: conv is first 4 bytes of KCP header. */
                uint32_t conv;
                memcpy(&conv, plain, 4);
                mtx_lock(&ln->sessions_mtx);
                xylem_rudp_conn_t* sess =
                    _rudp_find_session(ln, &peer_addr, conv);
                if (sess && sess->inbox) {
                    _rudp_inbox_push(sess, plain, plain_len);
                }
                mtx_unlock(&ln->sessions_mtx);
            }
        }

        if (plain != recv_buf) {
            free(plain);
        }
    }

    _rudp_listener_unref(ln);
}

xylem_rudp_conn_t* xylem_rudp_dial(
    const char*        host,
    uint16_t           port,
    xylem_rudp_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_dial");

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    /* Resolve hostname if needed. */
    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        uint64_t resolve_timeout = opts ? opts->connect_timeout_ms : 0;
        if (addr_resolve(host, port, resolve_timeout, &addrs, &count) != 0
            || count == 0) {
            xylem_loge("rudp dial: DNS resolution failed for %s", host);
            return NULL;
        }
        resolved_addr = addrs[0];
        free(addrs);
        uint16_t rport;
        addr_ntop(&resolved_addr, resolved_ip, sizeof(resolved_ip), &rport);
        dial_host = resolved_ip;
    }

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(
        dial_host, port_str, SOCK_DGRAM, &connected, true, false);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("rudp dial: socket creation failed for %s:%u", host, port);
        return NULL;
    }

    platform_socket_set_rcvbuf_max(fd, 0);

    xylem_rudp_conn_t* c =
        (xylem_rudp_conn_t*)calloc(1, sizeof(xylem_rudp_conn_t));
    if (!c) {
        platform_socket_close(fd);
        return NULL;
    }

    scheduler_t* sched = runtime_get_scheduler();
    uint32_t conv = _rudp_alloc_conv();

    atomic_store(&c->refcnt, 1);
    queue_init(&c->pending_out);
    c->fd        = fd;
    c->conv      = conv;
    c->peer_addr = resolved_addr;
    c->mode      = opts ? opts->mode : XYLEM_RUDP_STREAM;
    c->mtu       = (opts && opts->mtu > 0) ? (int)opts->mtu : RUDP_DEFAULT_MTU;

    c->waiter = iowait_create(fd);
    if (!c->waiter) {
        platform_socket_close(fd);
        free(c);
        return NULL;
    }

    if (_rudp_conn_init_mutexes(c) != 0) {
        iowait_destroy(c->waiter);
        platform_socket_close(fd);
        free(c);
        return NULL;
    }

    /* AES setup. */
    if (opts && opts->aes_key) {
        c->aes = xylem_aes256_create(opts->aes_key);
        if (!c->aes) {
            xylem_loge("rudp dial conv=%u: AES init failed", conv);
            _rudp_conn_destroy_mutexes(c);
            iowait_destroy(c->waiter);
            platform_socket_close(fd);
            free(c);
            return NULL;
        }
    }

    /* FEC setup. */
    if (opts && _rudp_init_fec(c, c->mtu, opts->fec_data,
                               opts->fec_parity) != 0) {
        xylem_loge("rudp dial conv=%u: FEC init failed", conv);
        xylem_aes256_destroy(c->aes);
        _rudp_conn_destroy_mutexes(c);
        iowait_destroy(c->waiter);
        platform_socket_close(fd);
        free(c);
        return NULL;
    }

    /* KCP setup. */
    c->kcp = _rudp_create_kcp(c, conv, opts);
    if (!c->kcp) {
        xylem_loge("rudp dial conv=%u: KCP create failed", conv);
        rudp_fec_enc_destroy(c->fec_enc);
        rudp_fec_dec_destroy(c->fec_dec);
        xylem_aes256_destroy(c->aes);
        _rudp_conn_destroy_mutexes(c);
        iowait_destroy(c->waiter);
        platform_socket_close(fd);
        free(c);
        return NULL;
    }

    /* Update timer. */
    c->update_timer = scheduler_timer_create(sched);
    if (!c->update_timer) {
        ikcp_release(c->kcp);
        rudp_fec_enc_destroy(c->fec_enc);
        rudp_fec_dec_destroy(c->fec_dec);
        xylem_aes256_destroy(c->aes);
        _rudp_conn_destroy_mutexes(c);
        iowait_destroy(c->waiter);
        platform_socket_close(fd);
        free(c);
        return NULL;
    }

    /* Handshake: loop sending SYN until ACK or timeout. */
    uint64_t hs_timeout = RUDP_DEFAULT_TIMEOUT_MS;
    if (opts && opts->connect_timeout_ms > 0) {
        hs_timeout = opts->connect_timeout_ms;
    }
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + hs_timeout;
    uint8_t syn[RUDP_HANDSHAKE_SIZE];
    _rudp_encode_handshake(syn, RUDP_HANDSHAKE_SYN, conv);

    uint8_t recv_buf[RUDP_HANDSHAKE_SIZE + RUDP_AES_IV_SIZE + 64];
    bool handshake_done = false;

    while (!handshake_done) {
        /* Send SYN. */
        xylem_mutex_lock(c->kcp_mu);
        (void)_rudp_encrypt_send(c, syn, RUDP_HANDSHAKE_SIZE);
        xylem_mutex_unlock(c->kcp_mu);
        (void)_rudp_drain_pending(c, false);

        /* Wait for response with retransmit timeout. */
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            xylem_loge("rudp dial conv=%u: handshake timeout", conv);
            _rudp_conn_unref(c);
            return NULL;
        }
        uint64_t wait_deadline = now + RUDP_SYN_RETRANSMIT_MS;
        if (wait_deadline > deadline) {
            wait_deadline = deadline;
        }
        iowait_set_rd_deadline(c->waiter, wait_deadline);
        iowait_result_t r = iowait_read(c->waiter);

        if (r == IOWAIT_CLOSED) {
            _rudp_conn_unref(c);
            return NULL;
        }
        if (r == IOWAIT_TIMEOUT) {
            /* Retransmit SYN on next iteration. */
            continue;
        }
        if (r != IOWAIT_READY) {
            _rudp_conn_unref(c);
            return NULL;
        }

        /* IOWAIT_READY: read the response. */
        ssize_t n = platform_socket_recv(
            fd, recv_buf, (int)sizeof(recv_buf));
        if (n <= 0) {
            continue;
        }

        /* Decrypt if needed. */
        void*  plain     = NULL;
        size_t plain_len = 0;
        if (_rudp_decrypt_packet(c->aes, recv_buf, (size_t)n,
                                 &plain, &plain_len) != 0) {
            continue;
        }

        uint8_t  hs_type;
        uint32_t hs_conv;
        if (_rudp_decode_handshake(plain, plain_len,
                                   &hs_type, &hs_conv) == 0 &&
            hs_type == RUDP_HANDSHAKE_ACK && hs_conv == conv) {
            handshake_done = true;
        }

        /* Free decrypted buffer if it was allocated. */
        if (plain != recv_buf) {
            free(plain);
        }
    }

    /* Clear read deadline used during handshake. */
    iowait_set_rd_deadline(c->waiter, 0);

    /* Start the KCP update timer. */
    scheduler_timer_set_ud_guard(
        c->update_timer, _rudp_conn_ref, _rudp_conn_unref);
    scheduler_timer_set_spawn(c->update_timer, true);
    scheduler_timer_start(
            c->update_timer, _rudp_update_timer_cb, c, 10, 0);
    _rudp_schedule_update(c);

    return c;
}

int xylem_rudp_read(xylem_rudp_conn_t* conn, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_read");

    if (!buf || len <= 0) {
        return -1;
    }

    if (atomic_load(&conn->closed)) {
        return -1;
    }
    /**
     * Hold a reference across the (parking) read so a concurrent
     * xylem_rudp_close cannot free the conn/inbox out from under us.
     */
    _rudp_conn_ref(conn);
    int ret;
    xylem_mutex_lock(conn->rd_mu);
    if (conn->listener) {
        ret = _rudp_session_read(conn, buf, len);
    } else {
        ret = _rudp_client_read(conn, buf, len);
    }
    xylem_mutex_unlock(conn->rd_mu);
    _rudp_conn_unref(conn);
    return ret;
}

int xylem_rudp_write(xylem_rudp_conn_t* conn, const void* data, int len) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_write");

    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    if (atomic_load(&conn->closed)) {
        return -1;
    }

    _rudp_conn_ref(conn);
    int ret = -1;
    if (!atomic_load(&conn->closed)) {
        xylem_mutex_lock(conn->wr_mu);
        xylem_mutex_lock(conn->kcp_mu);
        int rc = ikcp_send(conn->kcp, (const char*)data, len);
        if (rc >= 0) {
            ikcp_flush(conn->kcp);
            ret = 0;
        }
        xylem_mutex_unlock(conn->kcp_mu);
        if (ret == 0 && _rudp_drain_pending(conn, true) != 0) {
            ret = -1;
        }
        xylem_mutex_unlock(conn->wr_mu);
        if (ret == 0) {
            _rudp_schedule_update(conn);
        }
    }
    _rudp_conn_unref(conn);
    return ret;
}

xylem_rudp_listener_t* xylem_rudp_listen(
    const char*        host,
    uint16_t           port,
    xylem_rudp_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_listen");

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true, false);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("rudp listen: bind failed for %s:%u", host, port);
        return NULL;
    }

    xylem_rudp_listener_t* ln =
        (xylem_rudp_listener_t*)calloc(1, sizeof(xylem_rudp_listener_t));
    if (!ln) {
        platform_socket_close(fd);
        return NULL;
    }

    scheduler_t* sched = runtime_get_scheduler();

    ln->fd    = fd;
    ln->sched = sched;
    if (opts) {
        ln->opts = *opts;
    }

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    ln->write_mu = xylem_mutex_create();
    if (!ln->write_mu) {
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    /* Deep-copy AES key so caller can free original. */
    if (ln->opts.aes_key) {
        memcpy(ln->aes_key_buf, ln->opts.aes_key, 32);
        ln->opts.aes_key = ln->aes_key_buf;
        ln->aes = xylem_aes256_create(ln->aes_key_buf);
        if (!ln->aes) {
            xylem_loge("rudp listen: AES init failed");
            xylem_mutex_destroy(ln->write_mu);
            iowait_destroy(ln->waiter);
            platform_socket_close(fd);
            free(ln);
            return NULL;
        }
    }

    rbtree_init(&ln->sessions, _rudp_session_cmp_nn, _rudp_session_cmp_kn);
    if (mtx_init(&ln->sessions_mtx, mtx_plain) != thrd_success) {
        xylem_mutex_destroy(ln->write_mu);
        xylem_aes256_destroy(ln->aes);
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    /* Accept queue: a channel carrying accepted session pointers. */
    ln->accept_ch = xylem_channel_create();
    if (!ln->accept_ch) {
        mtx_destroy(&ln->sessions_mtx);
        xylem_mutex_destroy(ln->write_mu);
        xylem_aes256_destroy(ln->aes);
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    _rudp_listener_ref(ln); /* caller's reference */
    _rudp_listener_ref(ln); /* dispatcher's reference */

    /* Spawn the background dispatcher coroutine. */
    if (runtime_spawn(_rudp_dispatcher, ln) != 0) {
        _rudp_listener_unref(ln);
        _rudp_listener_unref(ln);
        return NULL;
    }

    return ln;
}

xylem_rudp_conn_t* xylem_rudp_accept(xylem_rudp_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_accept");

    _rudp_listener_ref(ln);
    xylem_rudp_conn_t* c = (xylem_rudp_conn_t*)xylem_channel_recv(ln->accept_ch);
    if (c) {
        _rudp_listener_ref(ln);
        c->accepted = true;
    }
    _rudp_listener_unref(ln);
    return c;
}

/**
 * Tear a session down exactly once (guarded by the `closed` exchange):
 * stop the update timer and wake any parked reader. Does NOT drop the
 * owner reference -- callers decide who owns that drop (see
 * xylem_rudp_close and _rudp_deferred_close). Idempotent: a second caller
 * loses the exchange and returns immediately.
 */
static void _rudp_conn_shutdown(xylem_rudp_conn_t* conn) {
    if (atomic_exchange(&conn->closed, true)) {
        return;
    }

    /**
     * Stop the update timer so no further ikcp_update fires; the timer
     * object is destroyed by the last _rudp_conn_unref.
     */
    if (conn->update_timer) {
        scheduler_timer_stop(conn->update_timer);
    }

    if (conn->listener) {
        bool drop_session_ref = false;
        mtx_lock(&conn->listener->sessions_mtx);
        if (conn->in_listener) {
            rbtree_remove(&conn->listener->sessions, &conn->listener_node);
            conn->in_listener = false;
            drop_session_ref = true;
        }
        mtx_unlock(&conn->listener->sessions_mtx);

        /**
         * Now close the inbox channel to wake a parked reader. The reader
         * holds a conn reference across its park, so the inbox and conn
         * stay alive until it drops that reference.
         */
        xylem_channel_close(conn->inbox);
        if (drop_session_ref) {
            _rudp_conn_unref(conn);
        }
    } else {
        iowait_close(conn->waiter);
    }
}

void xylem_rudp_close(xylem_rudp_conn_t* conn) {
    if (!conn) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_close");

    _rudp_conn_shutdown(conn);

    /* Drop the owner reference; the last reference out frees the conn. */
    _rudp_conn_unref(conn);
}

void xylem_rudp_close_listener(xylem_rudp_listener_t* ln) {
    if (!ln) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_close_listener");

    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    /* Wake the dispatcher if parked in iowait_read. */
    iowait_close(ln->waiter);

    /**
     * Close all active sessions. xylem_rudp_close re-acquires
     * sessions_mtx, so release it around each call.
     */
    mtx_lock(&ln->sessions_mtx);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_rudp_conn_t* sess =
            rbtree_entry(node, xylem_rudp_conn_t, listener_node);
        _rudp_conn_ref(sess);
        mtx_unlock(&ln->sessions_mtx);
        _rudp_conn_shutdown(sess);
        _rudp_conn_unref(sess);
        mtx_lock(&ln->sessions_mtx);
    }
    xylem_channel_close(ln->accept_ch);
    mtx_unlock(&ln->sessions_mtx);

    _rudp_listener_unref(ln);
}

void xylem_rudp_set_read_deadline(
    xylem_rudp_conn_t* conn, uint64_t deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_set_read_deadline");

    atomic_store(&conn->rd_deadline_ms, deadline_ms);
    if (!conn->listener && conn->waiter) {
        iowait_set_rd_deadline(conn->waiter, deadline_ms);
    }
}

void xylem_rudp_set_write_deadline(
    xylem_rudp_conn_t* conn, uint64_t deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_set_write_deadline");

    atomic_store(&conn->wr_deadline_ms, deadline_ms);
    if (!conn->listener && conn->waiter) {
        iowait_set_wr_deadline(conn->waiter, deadline_ms);
    }
}

int xylem_rudp_remote_addr(
    xylem_rudp_conn_t* conn, char* host, int hostlen, uint16_t* port) {
    RUNTIME_REQUIRE_COROUTINE("rudp", "xylem_rudp_remote_addr");

    return addr_ntop(&conn->peer_addr, host, (size_t)hostlen, port);
}
