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

#include "xylem/xylem-rudp.h"
#include "xylem/xylem-aes256.h"
#include "xylem/xylem-bswap.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-rbtree.h"
#include "xylem/xylem-udp.h"
#include "xylem/xylem-utils.h"

#include "rudp/kcp/ikcp.h"
#include "rudp/rudp-fec.h"

#include "platform/platform-socket.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* KCP recv buffer, large enough for one full KCP message. */
#define RUDP_RECV_BUF_SIZE 65536

/* Placeholder timeout for xylem_loop_start_timer before _rudp_schedule_update
 * overwrites it with the real value from ikcp_check. */
#define RUDP_TIMER_INIT_MS 100

/**
 * Handshake magic bytes to distinguish control packets from KCP data.
 * The first 4 bytes of a KCP packet are always the conv field, so we
 * use a magic prefix that cannot collide with valid conv values.
 */
#define RUDP_HANDSHAKE_SYN    0x01
#define RUDP_HANDSHAKE_ACK    0x02
#define RUDP_HANDSHAKE_MAGIC  0x58594C4D  /* "XYLM" */

/* Field sizes in handshake and KCP headers. */
#define RUDP_MAGIC_SIZE       4  /* sizeof(uint32_t) handshake magic */
#define RUDP_CONV_SIZE        4  /* sizeof(uint32_t) KCP conv field */
#define RUDP_TYPE_SIZE        1  /* handshake type byte */

/* Handshake packet layout: [magic:4][type:1][conv:4] = 9 bytes. */
#define RUDP_HANDSHAKE_SIZE   (RUDP_MAGIC_SIZE + RUDP_TYPE_SIZE + RUDP_CONV_SIZE)

/* Offsets within the handshake packet. */
#define RUDP_OFF_TYPE         RUDP_MAGIC_SIZE
#define RUDP_OFF_CONV         (RUDP_MAGIC_SIZE + RUDP_TYPE_SIZE)

/* Default timeout for client waiting for handshake ACK. */
#define RUDP_DEFAULT_HANDSHAKE_MS 5000

/* AES-256-CTR prepends a 16-byte IV to each encrypted packet. */
#define RUDP_AES_IV_SIZE 16

struct xylem_rudp_conn_s {
    ikcpcb*                kcp;
    xylem_udp_t*           udp;
    xylem_rudp_handler_t*  handler;
    xylem_rudp_server_t*   server;       /* non-NULL for server sessions */
    xylem_addr_t           peer_addr;
    void*                  userdata;
    _Atomic bool           handshake_done;
    _Atomic bool           closing;
    _Atomic int32_t        refcount;
    int                    close_err;
    const char*            close_errmsg;
    uint32_t               conv;
    int                    mtu;          /* effective MTU for FEC create */
    int                    fec_data;     /* per-session FEC data shards */
    int                    fec_parity;   /* per-session FEC parity shards */
    xylem_loop_t*          loop;
    xylem_loop_timer_t*    update_timer;
    xylem_loop_timer_t*    handshake_timer; /* client-side only */
    xylem_rbtree_node_t    server_node;
    rudp_fec_enc_t*        fec_enc;      /* NULL when FEC disabled */
    rudp_fec_dec_t*        fec_dec;      /* NULL when FEC disabled */
    xylem_aes256_t*        aes;          /* client-only; server uses server->aes */
};

struct xylem_rudp_server_s {
    xylem_udp_t*           udp;
    xylem_rudp_handler_t*  handler;
    xylem_rudp_opts_t      opts;
    xylem_loop_t*          loop;
    xylem_rbtree_t         sessions;
    void*                  userdata;
    xylem_aes256_t*        aes;            /* shared by all sessions */
    uint8_t                aes_key_buf[32]; /* owned copy of AES key */
    bool                   closing;
};

typedef struct _rudp_deferred_send_s {
    xylem_rudp_conn_t* rudp;
    size_t        len;
    char          data[];
} _rudp_deferred_send_t;

typedef struct {
    xylem_addr_t* addr;
    uint32_t      conv;
} _rudp_session_key_t;

/* Module-level conv allocator, lazily seeded on first use. */
static _Atomic uint32_t _rudp_next_conv = 0;

static uint32_t _rudp_alloc_conv(void) {
    uint32_t v = atomic_load(&_rudp_next_conv);
    if (v == 0) {
        uint32_t seed = (uint32_t)xylem_utils_getprng(1, 0x7FFFFFFF);
        atomic_compare_exchange_strong(&_rudp_next_conv, &v, seed);
    }
    return atomic_fetch_add_explicit(&_rudp_next_conv, 1,
                                     memory_order_relaxed);
}

static int _rudp_addr_cmp(const xylem_addr_t* a, const xylem_addr_t* b) {
    if (a->storage.ss_family != b->storage.ss_family) {
        return (int)a->storage.ss_family - (int)b->storage.ss_family;
    }
    if (a->storage.ss_family == AF_INET) {
        const struct sockaddr_in* sa = (const struct sockaddr_in*)&a->storage;
        const struct sockaddr_in* sb = (const struct sockaddr_in*)&b->storage;
        if (sa->sin_port != sb->sin_port) {
            return (int)ntohs(sa->sin_port) - (int)ntohs(sb->sin_port);
        }
        return memcmp(&sa->sin_addr, &sb->sin_addr, sizeof(sa->sin_addr));
    }
    if (a->storage.ss_family == AF_INET6) {
        const struct sockaddr_in6* sa =
            (const struct sockaddr_in6*)&a->storage;
        const struct sockaddr_in6* sb =
            (const struct sockaddr_in6*)&b->storage;
        if (sa->sin6_port != sb->sin6_port) {
            return (int)ntohs(sa->sin6_port) - (int)ntohs(sb->sin6_port);
        }
        return memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(sa->sin6_addr));
    }
    return 0;
}

static int _rudp_session_cmp(const xylem_addr_t* a_addr, uint32_t a_conv,
                             const xylem_addr_t* b_addr, uint32_t b_conv) {
    int rc = _rudp_addr_cmp(a_addr, b_addr);
    if (rc != 0) {
        return rc;
    }
    if (a_conv < b_conv) {
        return -1;
    }
    if (a_conv > b_conv) {
        return 1;
    }
    return 0;
}

static int _rudp_session_cmp_nn(const xylem_rbtree_node_t* a,
                                const xylem_rbtree_node_t* b) {
    const xylem_rudp_conn_t* ra =
        xylem_rbtree_entry(a, xylem_rudp_conn_t, server_node);
    const xylem_rudp_conn_t* rb =
        xylem_rbtree_entry(b, xylem_rudp_conn_t, server_node);
    return _rudp_session_cmp(&ra->peer_addr, ra->conv,
                             &rb->peer_addr, rb->conv);
}

static int _rudp_session_cmp_kn(const void* key,
                                const xylem_rbtree_node_t* node) {
    const _rudp_session_key_t* k = (const _rudp_session_key_t*)key;
    const xylem_rudp_conn_t* rudp =
        xylem_rbtree_entry(node, xylem_rudp_conn_t, server_node);
    return _rudp_session_cmp(k->addr, k->conv,
                             &rudp->peer_addr, rudp->conv);
}

static xylem_rudp_conn_t* _rudp_find_session(xylem_rudp_server_t* server,
                                        xylem_addr_t* addr,
                                        uint32_t conv) {
    _rudp_session_key_t key = { .addr = addr, .conv = conv };
    xylem_rbtree_node_t* node = xylem_rbtree_find(&server->sessions, &key);
    if (!node) {
        return NULL;
    }
    return xylem_rbtree_entry(node, xylem_rudp_conn_t, server_node);
}

/* Read a little-endian uint32 from a byte buffer (endian-safe). */
static inline uint32_t _rudp_read_le32(const void* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    if (xylem_utils_getendian() == XYLEM_ENDIAN_BE) {
        v = xylem_bswap(v);
    }
    return v;
}

/* Write a uint32 as little-endian into a byte buffer (endian-safe). */
static inline void _rudp_write_le32(void* p, uint32_t v) {
    if (xylem_utils_getendian() == XYLEM_ENDIAN_BE) {
        v = xylem_bswap(v);
    }
    memcpy(p, &v, sizeof(v));
}

static void _rudp_encode_handshake(uint8_t* buf, uint8_t type,
                                   uint32_t conv) {
    _rudp_write_le32(buf, RUDP_HANDSHAKE_MAGIC);
    buf[RUDP_OFF_TYPE] = type;
    _rudp_write_le32(buf + RUDP_OFF_CONV, conv);
}

static bool _rudp_decode_handshake(const void* data, size_t len,
                                   uint8_t* type, uint32_t* conv) {
    if (len != RUDP_HANDSHAKE_SIZE) {
        return false;
    }
    const uint8_t* buf = (const uint8_t*)data;
    uint32_t magic = _rudp_read_le32(buf);
    if (magic != RUDP_HANDSHAKE_MAGIC) {
        return false;
    }
    *type = buf[RUDP_OFF_TYPE];
    *conv = _rudp_read_le32(buf + RUDP_OFF_CONV);
    return true;
}

/**
 * Encrypt a UDP payload via AES-256-CTR and send it.
 * When aes is NULL the payload is sent unencrypted.
 */
static void _rudp_encrypted_send(xylem_udp_t* udp, xylem_addr_t* dest,
                                 xylem_aes256_t* aes,
                                 const void* data, size_t len) {
    if (!aes) {
        xylem_udp_send(udp, dest, data, len);
        return;
    }
    size_t enc_size = xylem_aes256_ctr_encrypt_size(len);
    uint8_t* enc_buf = (uint8_t*)malloc(enc_size);
    if (!enc_buf) {
        return;
    }
    int n = xylem_aes256_ctr_encrypt(
        aes, (const uint8_t*)data, len, enc_buf, enc_size);
    if (n > 0) {
        xylem_udp_send(udp, dest, enc_buf, (size_t)n);
    }
    free(enc_buf);
}

/**
 * Decrypt a received UDP payload via AES-256-CTR.
 * Returns the plaintext in *out_data / *out_len (caller must free).
 * When aes is NULL, sets *out_data to the original data (no alloc).
 * Returns true on success, false on failure.
 */
static bool _rudp_decrypt_packet(xylem_aes256_t* aes,
                                 void* data, size_t len,
                                 void** out_data, size_t* out_len,
                                 bool* out_allocated) {
    if (!aes) {
        *out_data      = data;
        *out_len       = len;
        *out_allocated = false;
        return true;
    }
    size_t dec_size = xylem_aes256_ctr_decrypt_size(len);
    if (dec_size == 0) {
        xylem_logw("rudp decrypt: packet too short (%zu bytes)", len);
        return false;
    }
    uint8_t* dec_buf = (uint8_t*)malloc(dec_size);
    if (!dec_buf) {
        return false;
    }
    int n = xylem_aes256_ctr_decrypt(
        aes, (const uint8_t*)data, len, dec_buf, dec_size);
    if (n <= 0) {
        free(dec_buf);
        return false;
    }
    *out_data      = dec_buf;
    *out_len       = (size_t)n;
    *out_allocated = true;
    return true;
}

/* KCP output callback: bridges KCP packets to UDP, via FEC if enabled. */
static int _rudp_kcp_output_cb(const char* buf, int len,
                               ikcpcb* kcp, void* user) {
    (void)kcp;
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)user;
    if (atomic_load(&rudp->closing)) {
        return -1;
    }

    /* Resolve AES context: client owns it, server session borrows it. */
    xylem_aes256_t* aes = rudp->aes;
    if (!aes && rudp->server) {
        aes = rudp->server->aes;
    }

    if (rudp->fec_enc) {
        int max_out = rudp_fec_enc_feed_size(rudp->fec_enc);
        rudp_fec_buf_t shards[RUDP_FEC_MAX_SHARDS];
        int n = rudp_fec_enc_feed(rudp->fec_enc, buf, (size_t)len,
                                  shards, max_out);
        for (int i = 0; i < n; i++) {
            _rudp_encrypted_send(rudp->udp, &rudp->peer_addr, aes,
                                 shards[i].data, shards[i].len);
        }
    } else {
        _rudp_encrypted_send(rudp->udp, &rudp->peer_addr, aes,
                             buf, (size_t)len);
    }
    return 0;
}

/**
 * Truncate to 32-bit; unsigned subtraction wraps mod 2^32, so
 * elapsed-time differences remain correct across overflow.
 */
static uint32_t _rudp_clock_ms(void) {
    return (uint32_t)(xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) &
                      0xFFFFFFFF);
}

static void _rudp_apply_opts(ikcpcb* kcp, const xylem_rudp_opts_t* opts,
                            bool fec_enabled, bool aes_enabled) {
    /* Always use fast mode: nodelay, 10ms interval, fast resend, no CC. */
    int interval = 10;
    ikcp_nodelay(kcp, 1, interval, 2, 1);
    ikcp_wndsize(kcp, 32, 128);

    int mtu = (opts && opts->mtu > 0) ? opts->mtu : 1400;
    /**
     * Reserve space for FEC header and AES IV so the total UDP
     * payload stays within the configured MTU.
     */
    if (fec_enabled) {
        mtu -= RUDP_FEC_HEADER_SIZE;
    }
    if (aes_enabled) {
        mtu -= RUDP_AES_IV_SIZE;
    }
    ikcp_setmtu(kcp, mtu);

    if (opts && opts->timeout_ms > 0) {
        kcp->dead_link = (IUINT32)(opts->timeout_ms / interval);
        if (kcp->dead_link == 0) {
            kcp->dead_link = 1;
        }
    }
}

static ikcpcb* _rudp_create_kcp(xylem_rudp_conn_t* rudp, uint32_t conv,
                                const xylem_rudp_opts_t* opts) {
    ikcpcb* kcp = ikcp_create(conv, rudp);
    if (!kcp) {
        return NULL;
    }
    ikcp_setoutput(kcp, _rudp_kcp_output_cb);
    bool fec = (rudp->fec_data > 0 && rudp->fec_parity > 0);
    bool aes = (opts && opts->aes_key != NULL);
    _rudp_apply_opts(kcp, opts, fec, aes);
    return kcp;
}

static void _rudp_conn_decref(xylem_rudp_conn_t* rudp) {
    if (atomic_fetch_sub(&rudp->refcount, 1) == 1) {
        free(rudp);
    }
}

/**
 * Deferred free so the session pointer stays valid through the
 * current loop iteration's callback chain.
 */
static void _rudp_free_cb(xylem_loop_t* loop, xylem_loop_post_t* req,
                          void* ud) {
    (void)loop;
    (void)req;
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)ud;
    xylem_loop_destroy_timer(rudp->update_timer);
    if (rudp->handshake_timer) {
        xylem_loop_destroy_timer(rudp->handshake_timer);
    }
    rudp_fec_enc_destroy(rudp->fec_enc);
    rudp_fec_dec_destroy(rudp->fec_dec);
    xylem_aes256_destroy(rudp->aes);
    _rudp_conn_decref(rudp);
}

/**
 * Drain the KCP recv queue and deliver complete messages via on_read.
 * Returns true if the session is still alive, false if closing was
 * triggered inside a callback.
 */
static bool _rudp_drain_recv(xylem_rudp_conn_t* rudp) {
    char buf[RUDP_RECV_BUF_SIZE];
    int  n;
    while ((n = ikcp_recv(rudp->kcp, buf, sizeof(buf))) > 0) {
        if (rudp->handler && rudp->handler->on_read) {
            rudp->handler->on_read(rudp, buf, (size_t)n);
        }
        if (atomic_load(&rudp->closing)) {
            return false;
        }
    }
    return true;
}

/**
 * Common path after ikcp_input: flush ACKs, deliver any complete
 * messages, and reschedule the update timer.
 */
static void _rudp_schedule_update(xylem_rudp_conn_t* rudp);

static void _rudp_input_complete(xylem_rudp_conn_t* rudp) {
    /**
     * Flush pending ACKs immediately rather than waiting for the next
     * update tick, so the peer gets timely RTT and window feedback.
     */
    ikcp_flush(rudp->kcp);

    /**
     * Fast path: deliver messages that fit in a single KCP segment
     * (len <= mss) immediately without waiting for the next update.
     */
    if (!_rudp_drain_recv(rudp)) {
        return;
    }

    _rudp_schedule_update(rudp);
}

static void _rudp_update_timeout_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)ud;
    if (atomic_load(&rudp->closing) || !rudp->kcp) {
        return;
    }

    uint32_t now = _rudp_clock_ms();
    ikcp_update(rudp->kcp, now);

    /* Check for dead link. */
    if (rudp->kcp->state == (IUINT32)-1) {
        xylem_logw("rudp conv=%u dead link detected", rudp->conv);
        rudp->close_err    = -1;
        rudp->close_errmsg = "dead link";
        xylem_rudp_close(rudp);
        return;
    }

    if (!_rudp_drain_recv(rudp)) {
        return;
    }

    _rudp_schedule_update(rudp);
}

/**
 * Query ikcp_check for the next update time and arm a one-shot timer.
 * Forward-declared above because _rudp_update_timeout_cb also calls it.
 */
static void _rudp_schedule_update(xylem_rudp_conn_t* rudp) {
    if (atomic_load(&rudp->closing) || !rudp->kcp) {
        return;
    }
    uint32_t now  = _rudp_clock_ms();
    uint32_t next = ikcp_check(rudp->kcp, now);
    uint64_t delay = (next <= now) ? 1 : (uint64_t)(next - now);
    xylem_loop_reset_timer(rudp->update_timer, delay);
}

static void _rudp_handshake_timeout_cb(xylem_loop_t* loop,
                                       xylem_loop_timer_t* timer,
                                       void* ud) {
    (void)loop;
    (void)timer;
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)ud;
    xylem_logw("rudp conv=%u handshake timed out", rudp->conv);
    rudp->close_err    = -1;
    rudp->close_errmsg = "handshake timeout";
    xylem_rudp_close(rudp);
}

/**
 * Unified receive path: decode FEC if enabled, then feed KCP.
 */
static void _rudp_recv_input(xylem_rudp_conn_t* rudp, void* data, size_t len) {
    if (!rudp->fec_dec) {
        ikcp_input(rudp->kcp, (const char*)data, (long)len);
        _rudp_input_complete(rudp);
        return;
    }

    int max_out = rudp_fec_dec_feed_size(rudp->fec_dec);
    rudp_fec_buf_t out[RUDP_FEC_MAX_SHARDS];
    int n = rudp_fec_dec_feed(rudp->fec_dec, data, len, out, max_out);

    for (int i = 0; i < n; i++) {
        ikcp_input(rudp->kcp, (const char*)out[i].data, (long)out[i].len);
    }

    if (n > 0) {
        _rudp_input_complete(rudp);
    }
}

static void _rudp_client_read_cb(xylem_udp_t* udp, void* data,
                                 size_t len, xylem_addr_t* addr) {
    (void)addr;
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)xylem_udp_get_userdata(udp);
    if (!rudp || atomic_load(&rudp->closing)) {
        return;
    }

    /* Decrypt at the outermost layer before any dispatch. */
    void*  plain     = NULL;
    size_t plain_len = 0;
    bool   allocated = false;
    if (!_rudp_decrypt_packet(rudp->aes, data, len,
                              &plain, &plain_len, &allocated)) {
        return;
    }

    if (!atomic_load(&rudp->handshake_done)) {
        uint8_t  type;
        uint32_t conv;
        if (_rudp_decode_handshake(plain, plain_len, &type, &conv) &&
            type == RUDP_HANDSHAKE_ACK && conv == rudp->conv) {
            atomic_store(&rudp->handshake_done, true);
            if (rudp->handshake_timer) {
                xylem_loop_stop_timer(rudp->handshake_timer);
            }
            xylem_logi("rudp conv=%u handshake complete", rudp->conv);
            _rudp_schedule_update(rudp);
            if (rudp->handler && rudp->handler->on_connect) {
                rudp->handler->on_connect(rudp);
            }
        }
        if (allocated) {
            free(plain);
        }
        return;
    }

    _rudp_recv_input(rudp, plain, plain_len);
    if (allocated) {
        free(plain);
    }
}

static void _rudp_client_close_cb(xylem_udp_t* udp, int err,
                                  const char* errmsg) {
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)xylem_udp_get_userdata(udp);
    if (!rudp) {
        return;
    }

    /**
     * Mark closing and stop timers to prevent _rudp_handshake_timeout_cb
     * or _rudp_update_timeout_cb from firing after the UDP socket is gone.
     * On Linux/macOS a connected UDP socket may receive ECONNREFUSED
     * (ICMP port unreachable) before the handshake timer fires.
     */
    atomic_store(&rudp->closing, true);
    xylem_loop_stop_timer(rudp->update_timer);
    if (rudp->handshake_timer) {
        xylem_loop_stop_timer(rudp->handshake_timer);
    }

    /* Propagate UDP-layer error only when RUDP has not set its own. */
    if (rudp->close_err == 0 && err != 0) {
        rudp->close_err    = err;
        rudp->close_errmsg = errmsg;
    }

    if (rudp->kcp) {
        ikcp_release(rudp->kcp);
        rudp->kcp = NULL;
    }
    if (rudp->handler && rudp->handler->on_close) {
        rudp->handler->on_close(rudp, rudp->close_err, rudp->close_errmsg);
    }
    xylem_loop_post(rudp->loop, _rudp_free_cb, rudp);
}

/* Helper to create FEC encoder/decoder pair from per-session params. */
static bool _rudp_init_fec(xylem_rudp_conn_t* rudp, int mtu) {
    if (rudp->fec_data <= 0 || rudp->fec_parity <= 0) {
        return true;
    }
    int effective_mtu = mtu > 0 ? mtu : 1400;
    rudp->fec_enc = rudp_fec_enc_create(rudp->fec_data, rudp->fec_parity,
                                        effective_mtu);
    if (!rudp->fec_enc) {
        return false;
    }
    rudp->fec_dec = rudp_fec_dec_create(rudp->fec_data, rudp->fec_parity,
                                        effective_mtu);
    if (!rudp->fec_dec) {
        rudp_fec_enc_destroy(rudp->fec_enc);
        rudp->fec_enc = NULL;
        return false;
    }
    return true;
}

/* Accept a new session from a SYN handshake. */
static void _rudp_accept_session(
    xylem_rudp_server_t* server, xylem_addr_t* addr, uint32_t conv) {
    xylem_rudp_conn_t* rudp = calloc(1, sizeof(*rudp));
    if (!rudp) {
        xylem_loge("rudp server: session alloc failed");
        return;
    }

    rudp->udp        = server->udp;
    rudp->handler    = server->handler;
    rudp->server     = server;
    rudp->peer_addr  = *addr;
    rudp->conv       = conv;
    rudp->loop       = server->loop;
    rudp->mtu        = server->opts.mtu > 0 ? server->opts.mtu : 1400;
    rudp->fec_data   = server->opts.fec_data;
    rudp->fec_parity = server->opts.fec_parity;

    rudp->update_timer = xylem_loop_create_timer(server->loop);

    rudp->kcp = _rudp_create_kcp(rudp, conv, &server->opts);
    if (!rudp->kcp) {
        xylem_loge("rudp conv=%u kcp creation failed", conv);
        xylem_loop_destroy_timer(rudp->update_timer);
        free(rudp);
        return;
    }

    if (!_rudp_init_fec(rudp, rudp->mtu)) {
        xylem_loge("rudp conv=%u fec init failed", conv);
        ikcp_release(rudp->kcp);
        xylem_loop_destroy_timer(rudp->update_timer);
        free(rudp);
        return;
    }

    atomic_store(&rudp->handshake_done, true);
    atomic_store(&rudp->refcount, 1);
    xylem_rbtree_insert(&server->sessions, &rudp->server_node);
    xylem_logi("rudp conv=%u session accepted (aes=%s fec=%d+%d)",
               conv, server->aes ? "on" : "off",
               rudp->fec_data, rudp->fec_parity);

    xylem_loop_start_timer(rudp->update_timer,
                           _rudp_update_timeout_cb, rudp,
                           RUDP_TIMER_INIT_MS, 0);
    _rudp_schedule_update(rudp);

    if (rudp->handler && rudp->handler->on_accept) {
        rudp->handler->on_accept(server, rudp);
    }
}

/* Dispatch a data packet (FEC or raw KCP) to the correct session. */
static void _rudp_server_dispatch_data(
    xylem_rudp_server_t* server, void* plain, size_t plain_len,
    xylem_addr_t* addr) {
    if (plain_len < RUDP_CONV_SIZE) {
        return;
    }

    const uint8_t* p = (const uint8_t*)plain;
    uint16_t maybe_fec_type = 0;
    if (plain_len >= RUDP_FEC_HEADER_SIZE) {
        maybe_fec_type = (uint16_t)((uint16_t)p[4] |
                                    ((uint16_t)p[5] << 8));
    }

    if (maybe_fec_type == RUDP_FEC_TYPE_DATA) {
        if (plain_len < RUDP_FEC_HEADER_SIZE + RUDP_CONV_SIZE) {
            return;
        }
        uint32_t conv = _rudp_read_le32(p + RUDP_FEC_HEADER_SIZE);
        xylem_rudp_conn_t* rudp = _rudp_find_session(server, addr, conv);
        if (rudp) {
            _rudp_recv_input(rudp, plain, plain_len);
        }
    } else if (maybe_fec_type == RUDP_FEC_TYPE_PARITY) {
        xylem_rbtree_node_t* node =
            xylem_rbtree_first(&server->sessions);
        while (node) {
            xylem_rudp_conn_t* rudp =
                xylem_rbtree_entry(node, xylem_rudp_conn_t, server_node);
            node = xylem_rbtree_next(node);
            if (_rudp_addr_cmp(&rudp->peer_addr, addr) == 0 &&
                rudp->fec_dec) {
                _rudp_recv_input(rudp, plain, plain_len);
            }
        }
    } else {
        uint32_t conv = _rudp_read_le32(plain);
        xylem_rudp_conn_t* rudp = _rudp_find_session(server, addr, conv);
        if (rudp) {
            ikcp_input(rudp->kcp, (const char*)plain, (long)plain_len);
            _rudp_input_complete(rudp);
        }
    }
}

/* Handle a SYN handshake: send ACK and accept a new session if needed. */
static void _rudp_server_handle_syn(
    xylem_rudp_server_t* server, xylem_udp_t* udp,
    xylem_addr_t* addr, uint32_t conv) {
    /* Send ACK regardless (client may have missed the first). */
    uint8_t ack[RUDP_HANDSHAKE_SIZE];
    _rudp_encode_handshake(ack, RUDP_HANDSHAKE_ACK, conv);
    _rudp_encrypted_send(udp, addr, server->aes, ack, RUDP_HANDSHAKE_SIZE);

    if (!_rudp_find_session(server, addr, conv)) {
        _rudp_accept_session(server, addr, conv);
    }
}

static void _rudp_server_read_cb(xylem_udp_t* udp, void* data,
                                 size_t len, xylem_addr_t* addr) {
    xylem_rudp_server_t* server =
        (xylem_rudp_server_t*)xylem_udp_get_userdata(udp);

    if (server->closing) {
        return;
    }

    /* Decrypt at the outermost layer before any dispatch. */
    void*  plain     = NULL;
    size_t plain_len = 0;
    bool   allocated = false;
    if (!_rudp_decrypt_packet(server->aes, data, len,
                              &plain, &plain_len, &allocated)) {
        return;
    }

    uint8_t  hs_type;
    uint32_t hs_conv;
    if (_rudp_decode_handshake(plain, plain_len, &hs_type, &hs_conv)) {
        if (hs_type == RUDP_HANDSHAKE_SYN) {
            _rudp_server_handle_syn(server, udp, addr, hs_conv);
        }
    } else {
        _rudp_server_dispatch_data(server, plain, plain_len, addr);
    }

    if (allocated) {
        free(plain);
    }
}

static void _rudp_server_close_cb(xylem_udp_t* udp, int err,
                                  const char* errmsg) {
    (void)err;
    (void)errmsg;
    xylem_rudp_server_t* server =
        (xylem_rudp_server_t*)xylem_udp_get_userdata(udp);
    xylem_aes256_destroy(server->aes);
    memset(server->aes_key_buf, 0, sizeof(server->aes_key_buf));
    free(server);
}

static xylem_udp_handler_t _rudp_client_udp_handler = {
    .on_read  = _rudp_client_read_cb,
    .on_close = _rudp_client_close_cb,
};

static xylem_udp_handler_t _rudp_server_udp_handler = {
    .on_read  = _rudp_server_read_cb,
    .on_close = _rudp_server_close_cb,
};

/**
 * Roll back a partially initialised dial session.
 * Each field is NULL-safe: calloc zeroes everything, so only
 * resources that were actually created get released.
 */
static void _rudp_dial_cleanup(xylem_rudp_conn_t* rudp,
                                xylem_udp_t* udp) {
    xylem_aes256_destroy(rudp->aes);
    rudp_fec_enc_destroy(rudp->fec_enc);
    rudp_fec_dec_destroy(rudp->fec_dec);
    if (rudp->kcp) {
        ikcp_release(rudp->kcp);
    }
    if (rudp->handshake_timer) {
        xylem_loop_destroy_timer(rudp->handshake_timer);
    }
    if (rudp->update_timer) {
        xylem_loop_destroy_timer(rudp->update_timer);
    }
    if (udp) {
        xylem_udp_set_userdata(udp, NULL);
        xylem_udp_close(udp);
    }
    free(rudp);
}

xylem_rudp_conn_t* xylem_rudp_dial(xylem_loop_t* loop,
                              xylem_addr_t* addr,
                              xylem_rudp_handler_t* handler,
                              xylem_rudp_opts_t* opts) {
    xylem_rudp_conn_t* rudp = calloc(1, sizeof(*rudp));
    if (!rudp) {
        return NULL;
    }

    uint32_t conv = _rudp_alloc_conv();

    rudp->handler    = handler;
    rudp->peer_addr  = *addr;
    rudp->conv       = conv;
    rudp->loop       = loop;
    rudp->mtu        = (opts && opts->mtu > 0) ? opts->mtu : 1400;
    rudp->fec_data   = opts ? opts->fec_data : 0;
    rudp->fec_parity = opts ? opts->fec_parity : 0;
    atomic_store(&rudp->refcount, 1);

    xylem_udp_t* udp = xylem_udp_dial(loop, addr,
                                      &_rudp_client_udp_handler);
    if (!udp) {
        free(rudp);
        return NULL;
    }

    rudp->udp = udp;
    xylem_udp_set_userdata(udp, rudp);

    rudp->update_timer    = xylem_loop_create_timer(loop);
    rudp->handshake_timer = xylem_loop_create_timer(loop);

    rudp->kcp = _rudp_create_kcp(rudp, conv, opts);
    if (!rudp->kcp) {
        xylem_loge("rudp conv=%u kcp creation failed", conv);
        _rudp_dial_cleanup(rudp, udp);
        return NULL;
    }

    if (!_rudp_init_fec(rudp, rudp->mtu)) {
        xylem_loge("rudp conv=%u fec init failed", conv);
        _rudp_dial_cleanup(rudp, udp);
        return NULL;
    }

    if (opts && opts->aes_key) {
        rudp->aes = xylem_aes256_create(opts->aes_key);
        if (!rudp->aes) {
            xylem_loge("rudp conv=%u aes init failed", conv);
            _rudp_dial_cleanup(rudp, udp);
            return NULL;
        }
    }

    xylem_logi("rudp conv=%u dial started (aes=%s fec=%d+%d)",
               conv, rudp->aes ? "on" : "off",
               rudp->fec_data, rudp->fec_parity);

    /* Initial start so _rudp_schedule_update can use reset. */
    xylem_loop_start_timer(rudp->update_timer, _rudp_update_timeout_cb,
                           rudp, RUDP_TIMER_INIT_MS, 0);

    uint8_t syn[RUDP_HANDSHAKE_SIZE];
    _rudp_encode_handshake(syn, RUDP_HANDSHAKE_SYN, conv);
    _rudp_encrypted_send(udp, NULL, rudp->aes, syn, RUDP_HANDSHAKE_SIZE);

    uint64_t hs_ms = (opts && opts->handshake_ms > 0)
                         ? opts->handshake_ms
                         : RUDP_DEFAULT_HANDSHAKE_MS;
    xylem_loop_start_timer(rudp->handshake_timer,
                           _rudp_handshake_timeout_cb, rudp,
                           hs_ms, 0);

    return rudp;
}

static void _rudp_deferred_send_cb(xylem_loop_t* loop,
                                    xylem_loop_post_t* req,
                                    void* ud) {
    (void)loop;
    (void)req;
    _rudp_deferred_send_t* ds = (_rudp_deferred_send_t*)ud;

    if (atomic_load(&ds->rudp->handshake_done) &&
        !atomic_load(&ds->rudp->closing)) {
        int rc = ikcp_send(ds->rudp->kcp, ds->data, (int)ds->len);
        if (rc >= 0) {
            ikcp_flush(ds->rudp->kcp);
            _rudp_schedule_update(ds->rudp);
        }
    }

    _rudp_conn_decref(ds->rudp);
    free(ds);
}

int xylem_rudp_send(xylem_rudp_conn_t* rudp, const void* data, size_t len) {
    if (!atomic_load(&rudp->handshake_done) ||
        atomic_load(&rudp->closing)) {
        xylem_logd("rudp conv=%u send rejected (handshake=%d closing=%d)",
                   rudp->conv,
                   (int)atomic_load(&rudp->handshake_done),
                   (int)atomic_load(&rudp->closing));
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    /* Cross-thread: copy data and post to loop thread. */
    if (!xylem_loop_is_loop_thread(rudp->loop)) {
        _rudp_deferred_send_t* ds = (_rudp_deferred_send_t*)malloc(
            sizeof(_rudp_deferred_send_t) + len);
        if (!ds) {
            return -1;
        }
        ds->rudp = rudp;
        ds->len  = len;
        memcpy(ds->data, data, len);

        atomic_fetch_add(&rudp->refcount, 1);
        if (xylem_loop_post(rudp->loop, _rudp_deferred_send_cb, ds) != 0) {
            _rudp_conn_decref(rudp);
            free(ds);
            return -1;
        }
        return 0;
    }

    /* Same thread: send directly via KCP. */
    int rc = ikcp_send(rudp->kcp, (const char*)data, (int)len);
    if (rc < 0) {
        return -1;
    }
    ikcp_flush(rudp->kcp);
    _rudp_schedule_update(rudp);
    return 0;
}

/**
 * Close logic that runs on the loop thread.  Extracted so that
 * _rudp_deferred_close_cb can call it directly instead of re-entering
 * xylem_rudp_close (which would re-post when loop->tid is unset).
 */
static void _rudp_do_close(xylem_rudp_conn_t* rudp) {
    if (atomic_load(&rudp->closing)) {
        return;
    }
    atomic_store(&rudp->closing, true);

    xylem_logi("rudp conv=%u closing", rudp->conv);

    xylem_loop_stop_timer(rudp->update_timer);

    if (rudp->handshake_timer) {
        xylem_loop_stop_timer(rudp->handshake_timer);
    }

    if (rudp->server) {
        xylem_rbtree_erase(&rudp->server->sessions, &rudp->server_node);

        if (rudp->kcp) {
            ikcp_release(rudp->kcp);
            rudp->kcp = NULL;
        }

        if (rudp->handler && rudp->handler->on_close) {
            rudp->handler->on_close(rudp, rudp->close_err,
                                    rudp->close_errmsg);
        }

        xylem_loop_post(rudp->loop, _rudp_free_cb, rudp);
    } else {
        xylem_udp_close(rudp->udp);
    }
}

static void _rudp_deferred_close_cb(xylem_loop_t* loop,
                                     xylem_loop_post_t* req,
                                     void* ud) {
    (void)loop;
    (void)req;
    xylem_rudp_conn_t* rudp = (xylem_rudp_conn_t*)ud;
    _rudp_do_close(rudp);
    _rudp_conn_decref(rudp);
}

void xylem_rudp_close(xylem_rudp_conn_t* rudp) {
    if (atomic_load(&rudp->closing)) {
        return;
    }

    /* Cross-thread: post to loop thread. */
    if (!xylem_loop_is_loop_thread(rudp->loop)) {
        xylem_logd("rudp conv=%u close from non-loop thread", rudp->conv);
        atomic_fetch_add(&rudp->refcount, 1);
        if (xylem_loop_post(rudp->loop, _rudp_deferred_close_cb, rudp) != 0) {
            _rudp_conn_decref(rudp);
        }
        return;
    }

    _rudp_do_close(rudp);
}

const xylem_addr_t* xylem_rudp_get_peer_addr(xylem_rudp_conn_t* rudp) {
    return &rudp->peer_addr;
}

xylem_loop_t* xylem_rudp_get_loop(xylem_rudp_conn_t* rudp) {
    return rudp->loop;
}

void* xylem_rudp_get_userdata(xylem_rudp_conn_t* rudp) {
    return rudp->userdata;
}

void xylem_rudp_set_userdata(xylem_rudp_conn_t* rudp, void* ud) {
    rudp->userdata = ud;
}

void xylem_rudp_conn_acquire(xylem_rudp_conn_t* rudp) {
    atomic_fetch_add(&rudp->refcount, 1);
}

void xylem_rudp_conn_release(xylem_rudp_conn_t* rudp) {
    _rudp_conn_decref(rudp);
}

xylem_rudp_server_t* xylem_rudp_listen(xylem_loop_t* loop,
                                       xylem_addr_t* addr,
                                       xylem_rudp_handler_t* handler,
                                       xylem_rudp_opts_t* opts) {
    xylem_rudp_server_t* server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    server->handler = handler;
    server->loop    = loop;
    if (opts) {
        server->opts = *opts;
    }

    /**
     * Deep-copy the AES key into server-owned storage so the caller
     * can free or reuse the original buffer after listen returns.
     */
    if (server->opts.aes_key) {
        memcpy(server->aes_key_buf, server->opts.aes_key, 32);
        server->opts.aes_key = server->aes_key_buf;
        server->aes = xylem_aes256_create(server->aes_key_buf);
        if (!server->aes) {
            xylem_loge("rudp server: aes init failed");
            free(server);
            return NULL;
        }
    }
    xylem_rbtree_init(&server->sessions, _rudp_session_cmp_nn,
                      _rudp_session_cmp_kn);

    xylem_udp_t* udp = xylem_udp_listen(loop, addr,
                                        &_rudp_server_udp_handler);
    if (!udp) {
        xylem_aes256_destroy(server->aes);
        memset(server->aes_key_buf, 0, sizeof(server->aes_key_buf));
        free(server);
        return NULL;
    }

    server->udp = udp;
    xylem_udp_set_userdata(udp, server);

    int fec_d = server->opts.fec_data;
    int fec_p = server->opts.fec_parity;
    xylem_logi("rudp server listening (aes=%s fec=%d+%d)",
               server->aes ? "on" : "off", fec_d, fec_p);

    return server;
}

void xylem_rudp_close_server(xylem_rudp_server_t* server) {
    if (server->closing) {
        return;
    }
    server->closing = true;

    xylem_logi("rudp server closing");

    while (!xylem_rbtree_empty(&server->sessions)) {
        xylem_rbtree_node_t* node = xylem_rbtree_first(&server->sessions);
        xylem_rudp_conn_t* rudp =
            xylem_rbtree_entry(node, xylem_rudp_conn_t, server_node);
        xylem_rudp_close(rudp);
    }

    /* _rudp_server_close_cb frees server. */
    xylem_udp_close(server->udp);
}

void* xylem_rudp_server_get_userdata(xylem_rudp_server_t* server) {
    return server->userdata;
}

void xylem_rudp_server_set_userdata(xylem_rudp_server_t* server, void* ud) {
    server->userdata = ud;
}
