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
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/addr.h"
#include "container/rbtree.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include "rudp-fec.h"
#include "kcp/ikcp.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define RUDP_DEFAULT_MTU         1400
#define RUDP_DEFAULT_TIMEOUT_MS  5000
#define RUDP_DEFAULT_DEADLINK_MS 30000
#define RUDP_RECV_BUF_SIZE       65536
#define RUDP_HANDSHAKE_MAGIC     0x58594C4D /* "XYLM" */
#define RUDP_HANDSHAKE_SYN       0x01
#define RUDP_HANDSHAKE_ACK       0x02
#define RUDP_HANDSHAKE_SIZE      9
#define RUDP_SYN_RETRANSMIT_MS   1000
#define RUDP_AES_IV_SIZE         16
#define RUDP_INBOX_CAP           64
#define RUDP_ACCEPT_CAP          16

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct _rudp_dgram_s {
    size_t len;
    char   data[];
} _rudp_dgram_t;

/**
 * Per-session ring buffer that the dispatcher pushes datagrams into
 * and the session read coroutine pops from. Uses cooperative park/wake
 * because the scheduler is single-threaded cooperative.
 */
typedef struct _rudp_inbox_s {
    _rudp_dgram_t** slots;
    uint32_t        cap;
    uint32_t        head;
    uint32_t        tail;
    mco_coro*       parked;
    scheduler_t*    sched;
    bool            closed;
} _rudp_inbox_t;

struct xylem_rudp_conn_s {
    ikcpcb*                kcp;
    platform_sock_t        fd;
    iowait_t*              waiter;
    xylem_rudp_mode_t      mode;
    addr_t                 peer_addr;
    _Atomic bool           closed;

    sched_timer_t*         update_timer;

    rudp_fec_enc_t*        fec_enc;
    rudp_fec_dec_t*        fec_dec;
    xylem_aes256_t*        aes;

    uint32_t               conv;
    int                    mtu;
    uint64_t               rd_deadline_ms;

    xylem_rudp_listener_t* listener;
    _rudp_inbox_t*         inbox;
    rbtree_node_t          listener_node;
};

struct xylem_rudp_listener_s {
    platform_sock_t        fd;
    iowait_t*              waiter;
    xylem_rudp_opts_t      opts;
    scheduler_t*           sched;
    rbtree_t               sessions;
    xylem_aes256_t*        aes;
    uint8_t                aes_key_buf[32];
    _Atomic bool           closed;

    xylem_rudp_conn_t**    accept_slots;
    uint32_t               accept_cap;
    uint32_t               accept_head;
    uint32_t               accept_tail;
    mco_coro*              accept_parked;
};

/* ------------------------------------------------------------------ */
/* Session tree key                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const addr_t* addr;
    uint32_t      conv;
} _rudp_session_key_t;

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static _Atomic uint32_t _rudp_next_conv = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* RBTree comparators                                                   */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* KCP output callback                                                 */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* KCP creation                                                        */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* FEC init                                                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* FEC receive path                                                    */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Update timer                                                        */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Inbox (server session receive buffer)                               */
/* ------------------------------------------------------------------ */

static _rudp_inbox_t* _rudp_inbox_create(scheduler_t* sched) {
    _rudp_inbox_t* ib =
        (_rudp_inbox_t*)calloc(1, sizeof(_rudp_inbox_t));
    if (!ib) {
        return NULL;
    }
    ib->slots = (_rudp_dgram_t**)calloc(
        RUDP_INBOX_CAP, sizeof(_rudp_dgram_t*));
    if (!ib->slots) {
        free(ib);
        return NULL;
    }
    ib->cap   = RUDP_INBOX_CAP;
    ib->sched = sched;
    return ib;
}

static void _rudp_inbox_destroy(_rudp_inbox_t* ib) {
    if (!ib) {
        return;
    }
    /* Drain any remaining datagrams. */
    while (ib->head != ib->tail) {
        uint32_t idx = ib->head % ib->cap;
        free(ib->slots[idx]);
        ib->head++;
    }
    free(ib->slots);
    free(ib);
}

/**
 * Push a datagram into the inbox. If full, oldest is dropped.
 * Wakes a parked reader coroutine.
 */
static void _rudp_inbox_push(_rudp_inbox_t* ib, const void* data,
                             size_t len) {
    if (ib->closed) {
        return;
    }

    /* Drop oldest if ring is full. */
    if (ib->tail - ib->head >= ib->cap) {
        uint32_t idx = ib->head % ib->cap;
        free(ib->slots[idx]);
        ib->head++;
    }

    _rudp_dgram_t* dgram =
        (_rudp_dgram_t*)malloc(sizeof(_rudp_dgram_t) + len);
    if (!dgram) {
        return;
    }
    dgram->len = len;
    memcpy(dgram->data, data, len);

    uint32_t idx = ib->tail % ib->cap;
    ib->slots[idx] = dgram;
    ib->tail++;

    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}

typedef struct {
    _rudp_inbox_t* ib;
} _rudp_inbox_park_ctx_t;

static bool _rudp_inbox_park_fn(mco_coro* co, void* arg) {
    _rudp_inbox_park_ctx_t* ctx = (_rudp_inbox_park_ctx_t*)arg;
    ctx->ib->parked = co;
    return true;
}

/**
 * Pop a datagram from the inbox. Parks the calling coroutine
 * if empty. Returns NULL if inbox is closed.
 */
static _rudp_dgram_t* _rudp_inbox_pop(_rudp_inbox_t* ib) {
    for (;;) {
        if (ib->closed) {
            return NULL;
        }
        if (ib->head != ib->tail) {
            uint32_t idx = ib->head % ib->cap;
            _rudp_dgram_t* dgram = ib->slots[idx];
            ib->slots[idx] = NULL;
            ib->head++;
            return dgram;
        }
        /* Park until a push or close wakes us. */
        _rudp_inbox_park_ctx_t ctx = { .ib = ib };
        scheduler_park(ib->sched, _rudp_inbox_park_fn, &ctx);
    }
}

static void _rudp_inbox_close(_rudp_inbox_t* ib) {
    if (!ib || ib->closed) {
        return;
    }
    ib->closed = true;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}

/* ------------------------------------------------------------------ */
/* Accept queue                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    xylem_rudp_listener_t* ln;
} _rudp_accept_park_ctx_t;

static bool _rudp_accept_park_fn(mco_coro* co, void* arg) {
    _rudp_accept_park_ctx_t* ctx = (_rudp_accept_park_ctx_t*)arg;
    ctx->ln->accept_parked = co;
    return true;
}

static void _rudp_accept_push(xylem_rudp_listener_t* ln,
                              xylem_rudp_conn_t* c) {
    /* Drop oldest if full (should not happen under normal load). */
    if (ln->accept_tail - ln->accept_head >= ln->accept_cap) {
        uint32_t idx = ln->accept_head % ln->accept_cap;
        xylem_rudp_close(ln->accept_slots[idx]);
        ln->accept_head++;
    }

    uint32_t idx = ln->accept_tail % ln->accept_cap;
    ln->accept_slots[idx] = c;
    ln->accept_tail++;

    if (ln->accept_parked) {
        mco_coro* co = ln->accept_parked;
        ln->accept_parked = NULL;
        scheduler_schedule(ln->sched, co);
    }
}

static xylem_rudp_conn_t* _rudp_accept_pop(xylem_rudp_listener_t* ln) {
    for (;;) {
        if (atomic_load_explicit(&ln->closed, memory_order_acquire)) {
            return NULL;
        }
        if (ln->accept_head != ln->accept_tail) {
            uint32_t idx = ln->accept_head % ln->accept_cap;
            xylem_rudp_conn_t* c = ln->accept_slots[idx];
            ln->accept_slots[idx] = NULL;
            ln->accept_head++;
            return c;
        }
        _rudp_accept_park_ctx_t ctx = { .ln = ln };
        scheduler_park(ln->sched, _rudp_accept_park_fn, &ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Client dial                                                         */
/* ------------------------------------------------------------------ */

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
            goto dial_fail;
        }
        uint64_t wait_deadline = now + RUDP_SYN_RETRANSMIT_MS;
        if (wait_deadline > deadline) {
            wait_deadline = deadline;
        }
        iowait_set_rd_deadline(c->waiter, wait_deadline);
        iowait_result_t r = iowait_read(c->waiter);

        if (r == IOWAIT_CLOSED) {
            goto dial_fail;
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

dial_fail:
    iowait_set_rd_deadline(c->waiter, 0);
    sched_timer_destroy(c->update_timer);
    ikcp_release(c->kcp);
    rudp_fec_enc_destroy(c->fec_enc);
    rudp_fec_dec_destroy(c->fec_dec);
    xylem_aes256_destroy(c->aes);
    iowait_destroy(c->waiter);
    platform_socket_close(fd);
    free(c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Client read (via iowait on connected socket)                        */
/* ------------------------------------------------------------------ */

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
            /* Update KCP clock on timeout to handle retransmits. */
            ikcp_update(c->kcp, _rudp_clock_ms());
            _rudp_schedule_update(c);
            return -1;
        }

        /* Read from connected socket. */
        ssize_t rn = platform_socket_recv(
            c->fd, recv_buf, (int)sizeof(recv_buf));
        if (rn <= 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                continue;
            }
            return -1;
        }

        /* Decrypt. */
        void*  plain     = NULL;
        size_t plain_len = 0;
        if (_rudp_decrypt_packet(c->aes, recv_buf, (size_t)rn,
                                 &plain, &plain_len) != 0) {
            continue;
        }

        /* Feed through FEC + KCP. */
        _rudp_recv_input(c, plain, plain_len);
        if (plain != recv_buf) {
            free(plain);
        }

        _rudp_schedule_update(c);
    }
}

/* ------------------------------------------------------------------ */
/* Server session read (via inbox from dispatcher)                     */
/* ------------------------------------------------------------------ */

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

        /* Pop a datagram from the inbox (parks if empty). */
        _rudp_dgram_t* dgram = _rudp_inbox_pop(c->inbox);
        if (!dgram) {
            return -1;
        }

        /* Feed into FEC + KCP. */
        _rudp_recv_input(c, dgram->data, dgram->len);
        free(dgram);
        _rudp_schedule_update(c);
    }
}

/* ------------------------------------------------------------------ */
/* Public read                                                         */
/* ------------------------------------------------------------------ */

int xylem_rudp_read(xylem_rudp_conn_t* c, void* buf, int len) {
    if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
        return -1;
    }
    if (c->listener) {
        return _rudp_session_read(c, buf, len);
    }
    return _rudp_client_read(c, buf, len);
}

/* ------------------------------------------------------------------ */
/* Client write                                                        */
/* ------------------------------------------------------------------ */

int xylem_rudp_write(xylem_rudp_conn_t* c, const void* data, int len) {
    if (atomic_load_explicit(&c->closed, memory_order_acquire)) {
        return -1;
    }
    if (!data || len <= 0) {
        return 0;
    }

    int rc = ikcp_send(c->kcp, (const char*)data, len);
    if (rc < 0) {
        return -1;
    }
    ikcp_flush(c->kcp);
    _rudp_schedule_update(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Message-mode send/recv                                              */
/* ------------------------------------------------------------------ */

int xylem_rudp_recv(xylem_rudp_conn_t* c, void* buf, int len) {
    /* Same as read for message mode. ikcp_recv returns one message. */
    return xylem_rudp_read(c, buf, len);
}

int xylem_rudp_send(xylem_rudp_conn_t* c, const void* data, int len) {
    return xylem_rudp_write(c, data, len);
}

/* ------------------------------------------------------------------ */
/* Server dispatcher coroutine                                         */
/* ------------------------------------------------------------------ */

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
                if (!_rudp_find_session(ln, &peer_addr, hs_conv)) {
                    xylem_rudp_conn_t* sess =
                        (xylem_rudp_conn_t*)calloc(
                            1, sizeof(xylem_rudp_conn_t));
                    if (sess) {
                        sess->fd        = ln->fd;
                        sess->conv      = hs_conv;
                        sess->peer_addr = peer_addr;
                        sess->listener  = ln;
                        sess->mode      = ln->opts.mode;
                        sess->mtu       = (ln->opts.mtu > 0)
                            ? (int)ln->opts.mtu : RUDP_DEFAULT_MTU;
                        sess->aes       = ln->aes;

                        if (_rudp_init_fec(sess, sess->mtu,
                                           ln->opts.fec_data,
                                           ln->opts.fec_parity) != 0) {
                            free(sess);
                            goto dispatch_next;
                        }

                        sess->kcp = _rudp_create_kcp(
                            sess, hs_conv, &ln->opts);
                        if (!sess->kcp) {
                            rudp_fec_enc_destroy(sess->fec_enc);
                            rudp_fec_dec_destroy(sess->fec_dec);
                            free(sess);
                            goto dispatch_next;
                        }

                        sess->inbox = _rudp_inbox_create(ln->sched);
                        if (!sess->inbox) {
                            ikcp_release(sess->kcp);
                            rudp_fec_enc_destroy(sess->fec_enc);
                            rudp_fec_dec_destroy(sess->fec_dec);
                            free(sess);
                            goto dispatch_next;
                        }

                        sess->update_timer =
                            sched_timer_create(ln->sched);
                        if (!sess->update_timer) {
                            _rudp_inbox_destroy(sess->inbox);
                            ikcp_release(sess->kcp);
                            rudp_fec_enc_destroy(sess->fec_enc);
                            rudp_fec_dec_destroy(sess->fec_dec);
                            free(sess);
                            goto dispatch_next;
                        }

                        sched_timer_start(
                            sess->update_timer,
                            _rudp_update_timer_cb, sess, 10, 0);
                        _rudp_schedule_update(sess);

                        rbtree_insert(
                            &ln->sessions, &sess->listener_node);
                        _rudp_accept_push(ln, sess);

                        xylem_logi("rudp listener: accepted conv=%u",
                                   hs_conv);
                    }
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
                    xylem_rudp_conn_t* sess =
                        _rudp_find_session(ln, &peer_addr, conv);
                    if (sess && sess->inbox) {
                        _rudp_inbox_push(
                            sess->inbox, plain, plain_len);
                    }
                } else if (fec_type == RUDP_FEC_TYPE_PARITY) {
                    /**
                     * Parity shards cannot be keyed by conv; deliver
                     * to every session from this peer so each FEC
                     * decoder can attempt recovery.
                     */
                    rbtree_node_t* nd = rbtree_min(&ln->sessions);
                    while (nd) {
                        xylem_rudp_conn_t* s = rbtree_entry(
                            nd, xylem_rudp_conn_t, listener_node);
                        nd = rbtree_next(nd);
                        if (memcmp(&s->peer_addr.storage,
                                   &peer_addr.storage,
                                   sizeof(struct sockaddr_storage))
                            == 0 && s->inbox) {
                            _rudp_inbox_push(
                                s->inbox, plain, plain_len);
                        }
                    }
                }
            } else if (plain_len >= 4) {
                /* No FEC: conv is first 4 bytes of KCP header. */
                uint32_t conv;
                memcpy(&conv, plain, 4);
                xylem_rudp_conn_t* sess =
                    _rudp_find_session(ln, &peer_addr, conv);
                if (sess && sess->inbox) {
                    _rudp_inbox_push(sess->inbox, plain, plain_len);
                }
            }
        }

dispatch_next:
        if (plain != recv_buf) {
            free(plain);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Server listen                                                       */
/* ------------------------------------------------------------------ */

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

    platform_socket_set_rcvbuf_max(fd, 0);

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

    /* Accept queue. */
    ln->accept_slots = (xylem_rudp_conn_t**)calloc(
        RUDP_ACCEPT_CAP, sizeof(xylem_rudp_conn_t*));
    if (!ln->accept_slots) {
        xylem_aes256_destroy(ln->aes);
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }
    ln->accept_cap = RUDP_ACCEPT_CAP;

    /* Spawn the background dispatcher coroutine. */
    runtime_spawn(_rudp_dispatcher, ln);

    xylem_logi("rudp listen: bound on %s:%u", host, port);
    return ln;
}

/* ------------------------------------------------------------------ */
/* Server accept                                                       */
/* ------------------------------------------------------------------ */

xylem_rudp_conn_t* xylem_rudp_accept(xylem_rudp_listener_t* ln) {
    return _rudp_accept_pop(ln);
}


/* ------------------------------------------------------------------ */
/* Close                                                               */
/* ------------------------------------------------------------------ */

void xylem_rudp_close(xylem_rudp_conn_t* c) {
    if (!c) {
        return;
    }
    if (atomic_exchange(&c->closed, true)) {
        return;
    }

    xylem_logi("rudp conv=%u: closing", c->conv);

    /* Stop update timer. */
    if (c->update_timer) {
        sched_timer_stop(c->update_timer);
        sched_timer_destroy(c->update_timer);
        c->update_timer = NULL;
    }

    if (c->listener) {
        /* Server session: close inbox, remove from tree. */
        _rudp_inbox_close(c->inbox);
        rbtree_remove(&c->listener->sessions, &c->listener_node);
    } else {
        /* Client: close iowait (wakes read). */
        iowait_close(c->waiter);
    }

    if (c->kcp) {
        ikcp_release(c->kcp);
        c->kcp = NULL;
    }

    rudp_fec_enc_destroy(c->fec_enc);
    rudp_fec_dec_destroy(c->fec_dec);
    c->fec_enc = NULL;
    c->fec_dec = NULL;

    if (!c->listener) {
        /* Client owns the socket and iowait. */
        iowait_destroy(c->waiter);
        platform_socket_close(c->fd);
        xylem_aes256_destroy(c->aes);
    } else {
        /* Server session does not own fd or AES (shared by listener). */
        _rudp_inbox_destroy(c->inbox);
        c->inbox = NULL;
    }

    free(c);
}

/* ------------------------------------------------------------------ */
/* Close listener                                                      */
/* ------------------------------------------------------------------ */

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

    /* Close all active sessions. */
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_rudp_conn_t* sess =
            rbtree_entry(node, xylem_rudp_conn_t, listener_node);
        xylem_rudp_close(sess);
    }

    /* Wake accept waiter. */
    if (ln->accept_parked) {
        mco_coro* co = ln->accept_parked;
        ln->accept_parked = NULL;
        scheduler_schedule(ln->sched, co);
    }

    /* Clean up. */
    xylem_aes256_destroy(ln->aes);
    memset(ln->aes_key_buf, 0, sizeof(ln->aes_key_buf));
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    free(ln->accept_slots);
    free(ln);
}

/* ------------------------------------------------------------------ */
/* Deadlines                                                           */
/* ------------------------------------------------------------------ */

void xylem_rudp_set_read_deadline(
    xylem_rudp_conn_t* c, uint64_t deadline_ms) {
    c->rd_deadline_ms = deadline_ms;
    /* For client connections, also set on iowait. */
    if (!c->listener && c->waiter) {
        iowait_set_rd_deadline(c->waiter, deadline_ms);
    }
}

void xylem_rudp_set_write_deadline(
    xylem_rudp_conn_t* c, uint64_t deadline_ms) {
    (void)c;
    (void)deadline_ms;
    /**
     * Write is non-blocking (ikcp_send + flush). The write deadline
     * is a no-op for RUDP since KCP buffers internally and the output
     * callback sends directly. Retained for API symmetry.
     */
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

int xylem_rudp_remote_addr(
    xylem_rudp_conn_t* c, char* host, int hostlen, uint16_t* port) {
    return addr_ntop(&c->peer_addr, host, (size_t)hostlen, port);
}

