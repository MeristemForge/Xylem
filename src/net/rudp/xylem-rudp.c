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
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/addr.h"
#include "container/rbtree.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "thrds.h"

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

typedef struct _rudp_dgram_s {
    size_t len;
    char   data[];
} _rudp_dgram_t;

struct xylem_rudp_conn_s {
    ikcpcb*                kcp;
    platform_sock_t        fd;
    iowait_t*              waiter;
    xylem_rudp_mode_t      mode;
    addr_t                 peer_addr;
    _Atomic bool           closed;
    _Atomic int32_t        refcnt;

    sched_timer_t*         update_timer;

    rudp_fec_enc_t*        fec_enc;
    rudp_fec_dec_t*        fec_dec;
    xylem_aes256_t*        aes;

    uint32_t               conv;
    int                    mtu;
    uint64_t               rd_deadline_ms;

    xylem_rudp_listener_t* listener;
    xylem_channel_t*       inbox;        /* server session datagram queue */
    _Atomic int32_t        inbox_len;    /* bounded-queue guard */
    rbtree_node_t          listener_node;
};

struct xylem_rudp_listener_s {
    platform_sock_t        fd;
    iowait_t*              waiter;
    xylem_rudp_opts_t      opts;
    scheduler_t*           sched;
    rbtree_t               sessions;
    mtx_t                  sessions_mtx;
    xylem_aes256_t*        aes;
    uint8_t                aes_key_buf[32];
    _Atomic bool           closed;

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
    uint32_t v = atomic_load_explicit(&_rudp_next_conv, memory_order_relaxed);
    if (v == 0) {
        uint32_t seed = (uint32_t)xylem_utils_getprng(1, 0x7FFFFFFF);
        atomic_compare_exchange_strong(&_rudp_next_conv, &v, seed);
    }
    return atomic_fetch_add_explicit(
        &_rudp_next_conv, 1, memory_order_relaxed);
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
static void _rudp_raw_send(xylem_rudp_conn_t* c, const void* data,
                           size_t len) {
    if (c->listener) {
        /* Server session: unconnected socket, use sendto. */
        socklen_t addrlen =
            (c->peer_addr.storage.ss_family == AF_INET6)
                ? (socklen_t)sizeof(struct sockaddr_in6)
                : (socklen_t)sizeof(struct sockaddr_in);
        platform_socket_sendto(
            c->fd, data, (int)len,
            &c->peer_addr.storage, addrlen);
    } else {
        /* Client: connected socket. */
        platform_socket_send(c->fd, data, (int)len);
    }
}

/**
 * AES encrypt then send. When AES is NULL, sends plaintext.
 * EAGAIN is acceptable here: KCP retransmits lost packets.
 */
static void _rudp_encrypt_send(xylem_rudp_conn_t* c, const void* data,
                               size_t len) {
    if (!c->aes) {
        _rudp_raw_send(c, data, len);
        return;
    }
    size_t enc_size = xylem_aes256_ctr_encrypt_size(len);
    uint8_t* enc_buf = (uint8_t*)malloc(enc_size);
    if (!enc_buf) {
        return;
    }
    int n = xylem_aes256_ctr_encrypt(
        c->aes, (const uint8_t*)data, len, enc_buf, enc_size);
    if (n > 0) {
        _rudp_raw_send(c, enc_buf, (size_t)n);
    }
    free(enc_buf);
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
    if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
        return -1;
    }

    if (c->fec_enc) {
        /* Feed each KCP segment through FEC; send resulting shards. */
        int max_out = rudp_fec_enc_feed_size(c->fec_enc);
        rudp_fec_buf_t shards[RUDP_FEC_MAX_SHARDS];
        int n = rudp_fec_enc_feed(
            c->fec_enc, buf, (size_t)len, shards, max_out);
        for (int i = 0; i < n; i++) {
            _rudp_encrypt_send(c, shards[i].data, shards[i].len);
        }
    } else {
        _rudp_encrypt_send(c, buf, (size_t)len);
    }
    return 0;
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
static void _rudp_recv_input(xylem_rudp_conn_t* c, void* data,
                             size_t len) {
    if (!c->fec_dec) {
        ikcp_input(c->kcp, (const char*)data, (long)len);
        ikcp_flush(c->kcp);
        return;
    }

    int max_out = rudp_fec_dec_feed_size(c->fec_dec);
    rudp_fec_buf_t out[RUDP_FEC_MAX_SHARDS];
    int n = rudp_fec_dec_feed(c->fec_dec, data, len, out, max_out);
    for (int i = 0; i < n; i++) {
        ikcp_input(c->kcp, (const char*)out[i].data, (long)out[i].len);
    }
    if (n > 0) {
        ikcp_flush(c->kcp);
    }
}

static void _rudp_schedule_update(xylem_rudp_conn_t* c);

static void _rudp_update_timer_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_rudp_conn_t* c = (xylem_rudp_conn_t*)ud;
    if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
        return;
    }

    uint32_t now = _rudp_clock_ms();
    ikcp_update(c->kcp, now);

    /* Detect dead link. */
    if (c->kcp->state == (IUINT32)-1) {
        xylem_logw("rudp conv=%u dead link", c->conv);
        xylem_rudp_close(c);
        return;
    }

    _rudp_schedule_update(c);
}

static void _rudp_schedule_update(xylem_rudp_conn_t* c) {
    if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
        return;
    }
    uint32_t now  = _rudp_clock_ms();
    uint32_t next = ikcp_check(c->kcp, now);
    uint64_t delay = (next <= now) ? 1 : (uint64_t)(next - now);
    sched_timer_reset(c->update_timer, delay);
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
    if (atomic_load_explicit(&sess->inbox_len, memory_order_relaxed)
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

    atomic_fetch_add_explicit(&sess->inbox_len, 1, memory_order_relaxed);
    if (xylem_channel_send(sess->inbox, dgram) != 0) {
        atomic_fetch_sub_explicit(&sess->inbox_len, 1, memory_order_relaxed);
        free(dgram);
    }
}

static void _rudp_conn_ref(xylem_rudp_conn_t* conn) {
    atomic_fetch_add_explicit(&conn->refcnt, 1, memory_order_relaxed);
}

/**
 * Drop a reference; the last one out performs the actual teardown.
 *
 * A reader parked in xylem_channel_recv / iowait_read holds a reference
 * across the park, so a concurrent xylem_rudp_close only marks the
 * session closed and wakes the reader -- the inbox, KCP state and the
 * conn itself stay alive until the woken reader drops its reference
 * here. Mirrors the refcounting used by tcp/tls/dtls connections.
 */
static void _rudp_conn_unref(xylem_rudp_conn_t* conn) {
    if (atomic_fetch_sub_explicit(&conn->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }

    if (conn->update_timer) {
        sched_timer_destroy(conn->update_timer);
        conn->update_timer = NULL;
    }

    if (conn->kcp) {
        ikcp_release(conn->kcp);
        conn->kcp = NULL;
    }

    rudp_fec_enc_destroy(conn->fec_enc);
    rudp_fec_dec_destroy(conn->fec_dec);
    conn->fec_enc = NULL;
    conn->fec_dec = NULL;

    if (!conn->listener) {
        /* Client owns its socket, waiter and AES context. */
        iowait_destroy(conn->waiter);
        platform_socket_close(conn->fd);
        xylem_aes256_destroy(conn->aes);
    } else {
        /* Server session shares the listener's socket and AES; it
         * only owns its inbox. */
        _rudp_inbox_destroy(conn->inbox);
        conn->inbox = NULL;
    }

    free(conn);
}

static int _rudp_client_read(xylem_rudp_conn_t* c, void* buf, int len) {
    char recv_buf[RUDP_RECV_BUF_SIZE];
    for (;;) {
        /* Try reading from KCP first. */
        int n = ikcp_recv(c->kcp, (char*)buf, len);
        if (n > 0) {
            return n;
        }

        if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
            return -1;
        }

        /* Set deadline for iowait. */
        if (c->rd_deadline_ms > 0) {
            iowait_set_rd_deadline(c->waiter, c->rd_deadline_ms);
        }

        iowait_result_t r = iowait_read(c->waiter);
        if (r == IOWAIT_CLOSED) {
            return -1;
        }
        if (r == IOWAIT_TIMEOUT) {
            ikcp_update(c->kcp, _rudp_clock_ms());
            _rudp_schedule_update(c);
            return -1;
        }

        /* ET poller: drain all queued datagrams until EAGAIN. */
        for (;;) {
            ssize_t rn = platform_socket_recv(
                c->fd, recv_buf, (int)sizeof(recv_buf));
            if (rn <= 0) {
                break;
            }

            void*  plain     = NULL;
            size_t plain_len = 0;
            if (_rudp_decrypt_packet(c->aes, recv_buf, (size_t)rn,
                                     &plain, &plain_len) != 0) {
                continue;
            }

            _rudp_recv_input(c, plain, plain_len);
            if (plain != recv_buf) {
                free(plain);
            }
        }

        _rudp_schedule_update(c);
    }
}

static int _rudp_session_read(xylem_rudp_conn_t* c, void* buf, int len) {
    for (;;) {
        /* Try KCP recv first. */
        int n = ikcp_recv(c->kcp, (char*)buf, len);
        if (n > 0) {
            return n;
        }

        if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
            return -1;
        }

        /* Pull a datagram from the inbox channel (parks if empty;
         * returns NULL once the channel is closed on teardown). */
        _rudp_dgram_t* dgram =
            (_rudp_dgram_t*)xylem_channel_recv(c->inbox);
        if (!dgram) {
            return -1;
        }
        atomic_fetch_sub_explicit(&c->inbox_len, 1, memory_order_relaxed);

        /* Feed into FEC + KCP. */
        _rudp_recv_input(c, dgram->data, dgram->len);
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

    atomic_store_explicit(&sess->refcnt, 1, memory_order_relaxed);
    sess->fd        = ln->fd;
    sess->conv      = hs_conv;
    sess->peer_addr = *peer_addr;
    sess->listener  = ln;
    sess->mode      = ln->opts.mode;
    sess->mtu       = (ln->opts.mtu > 0)
        ? (int)ln->opts.mtu : RUDP_DEFAULT_MTU;
    sess->aes       = ln->aes;

    if (_rudp_init_fec(sess, sess->mtu, ln->opts.fec_data,
                       ln->opts.fec_parity) != 0) {
        free(sess);
        return -1;
    }

    sess->kcp = _rudp_create_kcp(sess, hs_conv, &ln->opts);
    if (!sess->kcp) {
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    sess->inbox = xylem_channel_create();
    if (!sess->inbox) {
        ikcp_release(sess->kcp);
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    sess->update_timer = sched_timer_create(ln->sched);
    if (!sess->update_timer) {
        xylem_channel_destroy(sess->inbox);
        ikcp_release(sess->kcp);
        rudp_fec_enc_destroy(sess->fec_enc);
        rudp_fec_dec_destroy(sess->fec_dec);
        free(sess);
        return -1;
    }

    sched_timer_start(sess->update_timer, _rudp_update_timer_cb, sess, 10, 0);
    _rudp_schedule_update(sess);

    mtx_lock(&ln->sessions_mtx);
    rbtree_insert(&ln->sessions, &sess->listener_node);
    mtx_unlock(&ln->sessions_mtx);
    xylem_channel_send(ln->accept_ch, sess);

    xylem_logi("rudp listener: accepted conv=%u", hs_conv);
    return 0;
}

static void _rudp_dispatcher(void* arg) {
    xylem_rudp_listener_t* ln = (xylem_rudp_listener_t*)arg;
    char recv_buf[RUDP_RECV_BUF_SIZE];

    for (;;) {
        if (atomic_load_explicit(&ln->closed, memory_order_acquire)) {
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
                    /* Hold the lock across find+push so the session
                     * cannot be removed and freed between lookup and
                     * inbox push. _rudp_inbox_push only enqueues (it
                     * never parks), so this critical section is short. */
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
}

xylem_rudp_conn_t* xylem_rudp_dial(
    const char*        host,
    uint16_t           port,
    xylem_rudp_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    /* Resolve hostname if needed. */
    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        if (addr_resolve(host, port, &addrs, &count) != 0 || count == 0) {
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
        dial_host, port_str, SOCK_DGRAM, &connected, true);
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

    atomic_store_explicit(&c->refcnt, 1, memory_order_relaxed);
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

    /* AES setup. */
    if (opts && opts->aes_key) {
        c->aes = xylem_aes256_create(opts->aes_key);
        if (!c->aes) {
            xylem_loge("rudp dial conv=%u: AES init failed", conv);
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
        iowait_destroy(c->waiter);
        platform_socket_close(fd);
        free(c);
        return NULL;
    }

    /* Update timer. */
    c->update_timer = sched_timer_create(sched);
    if (!c->update_timer) {
        ikcp_release(c->kcp);
        rudp_fec_enc_destroy(c->fec_enc);
        rudp_fec_dec_destroy(c->fec_dec);
        xylem_aes256_destroy(c->aes);
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
        _rudp_encrypt_send(c, syn, RUDP_HANDSHAKE_SIZE);

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
    sched_timer_start(
        c->update_timer, _rudp_update_timer_cb, c, 10, 0);
    _rudp_schedule_update(c);

    xylem_logi("rudp dial conv=%u: connected to %s:%u", conv, host, port);
    return c;
}

int xylem_rudp_read(xylem_rudp_conn_t* conn, void* buf, int len) {
    if (atomic_load_explicit(&conn->closed, memory_order_acquire)) {
        return -1;
    }
    /* Hold a reference across the (parking) read so a concurrent
     * xylem_rudp_close cannot free the conn/inbox out from under us. */
    _rudp_conn_ref(conn);
    int ret;
    if (conn->listener) {
        ret = _rudp_session_read(conn, buf, len);
    } else {
        ret = _rudp_client_read(conn, buf, len);
    }
    _rudp_conn_unref(conn);
    return ret;
}

int xylem_rudp_write(xylem_rudp_conn_t* conn, const void* data, int len) {
    if (atomic_load_explicit(&conn->closed, memory_order_acquire)) {
        return -1;
    }
    if (!data || len <= 0) {
        return 0;
    }

    _rudp_conn_ref(conn);
    int ret = -1;
    if (!atomic_load_explicit(&conn->closed, memory_order_acquire)) {
        int rc = ikcp_send(conn->kcp, (const char*)data, len);
        if (rc >= 0) {
            ikcp_flush(conn->kcp);
            _rudp_schedule_update(conn);
            ret = 0;
        }
    }
    _rudp_conn_unref(conn);
    return ret;
}

xylem_rudp_listener_t* xylem_rudp_listen(
    const char*        host,
    uint16_t           port,
    xylem_rudp_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true);
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

    /* Deep-copy AES key so caller can free original. */
    if (ln->opts.aes_key) {
        memcpy(ln->aes_key_buf, ln->opts.aes_key, 32);
        ln->opts.aes_key = ln->aes_key_buf;
        ln->aes = xylem_aes256_create(ln->aes_key_buf);
        if (!ln->aes) {
            xylem_loge("rudp listen: AES init failed");
            iowait_destroy(ln->waiter);
            platform_socket_close(fd);
            free(ln);
            return NULL;
        }
    }

    rbtree_init(&ln->sessions, _rudp_session_cmp_nn, _rudp_session_cmp_kn);
    mtx_init(&ln->sessions_mtx, mtx_plain);

    /* Accept queue: a channel carrying accepted session pointers. */
    ln->accept_ch = xylem_channel_create();
    if (!ln->accept_ch) {
        mtx_destroy(&ln->sessions_mtx);
        xylem_aes256_destroy(ln->aes);
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    /* Spawn the background dispatcher coroutine. */
    runtime_spawn(_rudp_dispatcher, ln);

    xylem_logi("rudp listen: bound on %s:%u", host, port);
    return ln;
}

xylem_rudp_conn_t* xylem_rudp_accept(xylem_rudp_listener_t* ln) {
    xylem_rudp_conn_t* c = (xylem_rudp_conn_t*)xylem_channel_recv(ln->accept_ch);
    return c;
}

void xylem_rudp_close(xylem_rudp_conn_t* conn) {
    if (!conn) {
        return;
    }
    if (atomic_exchange(&conn->closed, true)) {
        return;
    }

    xylem_logi("rudp conv=%u: closing", conn->conv);

    /* Stop the update timer so no further ikcp_update fires; the timer
     * object is destroyed by the last _rudp_conn_unref. */
    if (conn->update_timer) {
        sched_timer_stop(conn->update_timer);
    }

    if (conn->listener) {
        /* Unlink from the session tree FIRST, under the lock, so the
         * dispatcher can no longer find this session and therefore
         * cannot xylem_channel_send() into the inbox after we close
         * it (send-on-closed aborts, Go-style). The dispatcher does
         * find+send under the same sessions_mtx, so once the remove
         * commits, no further send can target this inbox. */
        mtx_lock(&conn->listener->sessions_mtx);
        rbtree_remove(&conn->listener->sessions, &conn->listener_node);
        mtx_unlock(&conn->listener->sessions_mtx);

        /* Now close the inbox channel to wake a parked reader. The
         * reader's in-flight recv holds a channel reference, so the
         * channel stays alive until _rudp_conn_unref drains+destroys
         * it. The reader also holds a conn reference across its park,
         * so conn survives until it drops that reference. */
        xylem_channel_close(conn->inbox);
    } else {
        iowait_close(conn->waiter);
    }

    /* Drop the owner reference; the last reference out frees the conn. */
    _rudp_conn_unref(conn);
}

void xylem_rudp_close_listener(xylem_rudp_listener_t* ln) {
    if (!ln) {
        return;
    }
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    xylem_logi("rudp listener: closing");

    /* Wake the dispatcher if parked in iowait_read. */
    iowait_close(ln->waiter);

    /* Close all active sessions. xylem_rudp_close re-acquires
     * sessions_mtx, so release it around each call. */
    mtx_lock(&ln->sessions_mtx);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_rudp_conn_t* sess =
            rbtree_entry(node, xylem_rudp_conn_t, listener_node);
        mtx_unlock(&ln->sessions_mtx);
        xylem_rudp_close(sess);
        mtx_lock(&ln->sessions_mtx);
    }
    mtx_unlock(&ln->sessions_mtx);

    /* Wake the accept waiter and release the accept channel. Any
     * session still queued in the channel was already closed by the
     * session-teardown loop above (it was in the rbtree), so destroy
     * only frees the channel's node wrappers. */
    xylem_channel_destroy(ln->accept_ch);

    /* Clean up. */
    xylem_aes256_destroy(ln->aes);
    memset(ln->aes_key_buf, 0, sizeof(ln->aes_key_buf));
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    mtx_destroy(&ln->sessions_mtx);
    free(ln);
}

void xylem_rudp_set_read_deadline(
    xylem_rudp_conn_t* conn, uint64_t deadline_ms) {
    conn->rd_deadline_ms = deadline_ms;
    if (!conn->listener && conn->waiter) {
        iowait_set_rd_deadline(conn->waiter, deadline_ms);
    }
}

void xylem_rudp_set_write_deadline(
    xylem_rudp_conn_t* conn, uint64_t deadline_ms) {
    (void)conn;
    (void)deadline_ms;
}

int xylem_rudp_remote_addr(
    xylem_rudp_conn_t* conn, char* host, int hostlen, uint16_t* port) {
    return addr_ntop(&conn->peer_addr, host, (size_t)hostlen, port);
}
