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

#include "xylem/net/xylem-http.h"

#include "ws.h"
#include "ws-frame.h"
#include "ws-handshake.h"
#include "ws-utf8.h"

#include <stdlib.h>
#include <string.h>

static void _ws_opts_apply(xylem_ws_conn_t* conn, const xylem_ws_opts_t* opts) {
    if (opts) {
        conn->max_msg_size = opts->max_msg_size ? opts->max_msg_size
                                                : WS_DEFAULT_MAX_MSG_SIZE;
        conn->fragment_threshold = opts->fragment_threshold ? opts->fragment_threshold
                                                           : WS_DEFAULT_FRAGMENT_THRESHOLD;
        conn->close_timeout_ms = opts->close_timeout_ms ? opts->close_timeout_ms
                                                        : WS_DEFAULT_CLOSE_TIMEOUT;
        if (opts->permessage_deflate) {
            conn->deflate_requested        = true;
            conn->deflate_context_takeover = opts->deflate_context_takeover;
        }
    } else {
        conn->max_msg_size       = WS_DEFAULT_MAX_MSG_SIZE;
        conn->fragment_threshold = WS_DEFAULT_FRAGMENT_THRESHOLD;
        conn->close_timeout_ms   = WS_DEFAULT_CLOSE_TIMEOUT;
    }
}

xylem_ws_conn_t* ws_conn_create(http_transport_t transport,
                                 bool is_client,
                                 const xylem_ws_opts_t* opts) {
    xylem_ws_conn_t* conn = (xylem_ws_conn_t*)calloc(1, sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    conn->transport = transport;
    conn->is_client = is_client;
    conn->recv_buf  = (uint8_t*)malloc(WS_RECV_BUF_INIT);
    if (!conn->recv_buf) {
        free(conn);
        return NULL;
    }
    conn->recv_cap = WS_RECV_BUF_INIT;

    _ws_opts_apply(conn, opts);
    return conn;
}

void ws_conn_free(xylem_ws_conn_t* conn) {
    if (!conn) {
        return;
    }
    ws_deflate_cleanup(&conn->deflate_ctx);
    free(conn->recv_buf);
    free(conn->frag_buf);
    free(conn);
}


static int _ws_write_frame(xylem_ws_conn_t* conn, bool fin, uint8_t opcode,
                           const void* data, size_t len, bool rsv1) {
    uint8_t hdr_buf[14];
    uint8_t mask_key[4] = {0};

    if (conn->is_client) {
        uint32_t r = (uint32_t)rand();
        memcpy(mask_key, &r, 4);
    }

    size_t hdr_len = ws_frame_encode_header(hdr_buf, fin, opcode,
                                            conn->is_client, mask_key, len);

    if (rsv1) {
        hdr_buf[0] |= 0x40;
    }

    int n = conn->transport.write(conn->transport.conn, hdr_buf, (int)hdr_len);
    if (n < 0) {
        return -1;
    }

    if (len > 0) {
        if (conn->is_client) {
            uint8_t* masked = (uint8_t*)malloc(len);
            if (!masked) {
                return -1;
            }
            memcpy(masked, data, len);
            ws_frame_apply_mask(masked, len, mask_key, 0);
            n = conn->transport.write(conn->transport.conn, masked, (int)len);
            free(masked);
        } else {
            n = conn->transport.write(conn->transport.conn, data, (int)len);
        }
        if (n < 0) {
            return -1;
        }
    }
    return 0;
}


int xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                  const void* data, size_t len) {
    if (!conn || conn->close_sent || conn->close_received) {
        return -1;
    }
    if (opcode != XYLEM_WS_TEXT && opcode != XYLEM_WS_BINARY) {
        return -1;
    }

    const uint8_t* p = (const uint8_t*)data;
    size_t         send_len = len;
    void*          compressed = NULL;
    bool           use_deflate = conn->deflate_ctx.active;

    if (use_deflate) {
        size_t comp_len = 0;
        if (ws_deflate_compress(&conn->deflate_ctx, data, len,
                                &compressed, &comp_len) != 0) {
            return -1;
        }
        p        = (const uint8_t*)compressed;
        send_len = comp_len;
    }

    size_t threshold = conn->fragment_threshold;
    int    result    = 0;

    if (send_len <= threshold) {
        result = _ws_write_frame(conn, true, (uint8_t)opcode,
                                 p, send_len, use_deflate);
    } else {
        size_t offset = 0;
        bool   first  = true;
        while (offset < send_len) {
            size_t chunk = send_len - offset;
            if (chunk > threshold) {
                chunk = threshold;
            }
            bool    fin = (offset + chunk >= send_len);
            uint8_t op  = first ? (uint8_t)opcode : 0x0;
            /* RSV1 only on the first frame. */
            bool rsv1 = first && use_deflate;

            if (_ws_write_frame(conn, fin, op, p + offset, chunk, rsv1) != 0) {
                result = -1;
                break;
            }

            offset += chunk;
            first = false;
        }
    }

    free(compressed);
    return result;
}


int xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len) {
    if (!conn || conn->close_sent || conn->close_received) {
        return -1;
    }
    if (len > 125) {
        return -1;
    }
    return _ws_write_frame(conn, true, 0x9, data, len, false);
}

static int _ws_send_close_frame(xylem_ws_conn_t* conn, uint16_t code,
                                const char* reason, size_t reason_len) {
    uint8_t payload[125];
    int plen = ws_frame_close_encode(code, reason, reason_len,
                                     payload, sizeof(payload));
    if (plen < 0) {
        plen = 0;
    }
    return _ws_write_frame(conn, true, 0x8, payload, (size_t)plen, false);
}


static int _ws_ensure_recv_buf(xylem_ws_conn_t* conn, size_t needed) {
    if (conn->recv_cap >= needed) {
        return 0;
    }
    size_t new_cap = conn->recv_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    uint8_t* nb = (uint8_t*)realloc(conn->recv_buf, new_cap);
    if (!nb) {
        return -1;
    }
    conn->recv_buf = nb;
    conn->recv_cap = new_cap;
    return 0;
}

static int _ws_frag_append(xylem_ws_conn_t* conn, const void* data, size_t len) {
    size_t needed = conn->frag_len + len;
    if (conn->max_msg_size && needed > conn->max_msg_size) {
        return -1;
    }
    if (needed > conn->frag_cap) {
        size_t new_cap = conn->frag_cap ? conn->frag_cap : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t* nb = (uint8_t*)realloc(conn->frag_buf, new_cap);
        if (!nb) {
            return -1;
        }
        conn->frag_buf = nb;
        conn->frag_cap = new_cap;
    }
    memcpy(conn->frag_buf + conn->frag_len, data, len);
    conn->frag_len += len;
    return 0;
}

int xylem_ws_recv(xylem_ws_conn_t* conn, xylem_ws_msg_t* msg) {
    if (!conn || !msg) {
        return -1;
    }
    if (conn->close_received) {
        return -1;
    }

    for (;;) {
        while (conn->recv_len > 0) {
            ws_frame_header_t fh;
            int rc = ws_frame_decode_header(conn->recv_buf, conn->recv_len, &fh);
            if (rc == -1) {
                break;
            }
            if (rc == -2) {
                conn->close_code = 1002;
                return -1;
            }

            /* RSV1 is only valid when permessage-deflate is negotiated. */
            if (fh.rsv1 && !conn->deflate_ctx.active) {
                conn->close_code = 1002;
                return -1;
            }

            size_t frame_total = fh.header_size + (size_t)fh.payload_len;
            if (conn->recv_len < frame_total) {
                break;
            }

            uint8_t* payload = conn->recv_buf + fh.header_size;
            if (fh.masked) {
                ws_frame_apply_mask(payload, (size_t)fh.payload_len, fh.mask_key, 0);
            }

            size_t remaining = conn->recv_len - frame_total;
            if (remaining > 0) {
                memmove(conn->recv_buf, conn->recv_buf + frame_total, remaining);
            }
            conn->recv_len = remaining;

            if (fh.opcode == 0x8) { /* Close */
                uint16_t code; const char* reason; size_t rlen;
                ws_frame_close_decode(payload, (size_t)fh.payload_len,
                                      &code, &reason, &rlen);
                conn->close_code = code;
                conn->close_received = true;
                if (!conn->close_sent) {
                    _ws_send_close_frame(conn, code, reason, rlen);
                    conn->close_sent = true;
                }
                return -1;
            } else if (fh.opcode == 0x9) { /* Ping -> auto-pong */
                _ws_write_frame(conn, true, 0xA, payload, (size_t)fh.payload_len, false);
            } else if (fh.opcode == 0xA) { /* Pong -> discard */
                /* noop */
            } else if (fh.opcode == 0x0) { /* Continuation */
                if (!conn->frag_active) {
                    conn->close_code = 1002;
                    return -1;
                }
                if (_ws_frag_append(conn, payload, (size_t)fh.payload_len) != 0) {
                    conn->close_code = 1009;
                    return -1;
                }
                if (fh.fin) {
                    void*  msg_data = conn->frag_buf;
                    size_t msg_len  = conn->frag_len;

                    /* Decompress reassembled fragments if compressed. */
                    if (conn->frag_compressed && conn->deflate_ctx.active) {
                        void*  dec     = NULL;
                        size_t dec_len = 0;
                        if (ws_deflate_decompress(&conn->deflate_ctx,
                                                  conn->frag_buf, conn->frag_len,
                                                  &dec, &dec_len,
                                                  conn->max_msg_size) != 0) {
                            conn->close_code = 1009;
                            return -1;
                        }
                        free(conn->frag_buf);
                        msg_data = dec;
                        msg_len  = dec_len;
                    }

                    if (conn->frag_opcode == 0x1) {
                        if (ws_utf8_validate(msg_data, msg_len) != 0) {
                            free(msg_data);
                            conn->close_code = 1007;
                            return -1;
                        }
                    }
                    msg->opcode = (xylem_ws_opcode_t)conn->frag_opcode;
                    msg->data   = msg_data;
                    msg->len    = msg_len;
                    conn->frag_buf      = NULL;
                    conn->frag_len      = 0;
                    conn->frag_cap      = 0;
                    conn->frag_active   = false;
                    conn->frag_compressed = false;
                    return 0;
                }
            } else { /* Text or Binary */
                if (conn->frag_active) {
                    conn->close_code = 1002;
                    return -1;
                }
                if (fh.fin) {
                    if (conn->max_msg_size && fh.payload_len > conn->max_msg_size) {
                        conn->close_code = 1009;
                        return -1;
                    }

                    void*  msg_data;
                    size_t msg_len;

                    if (fh.rsv1 && conn->deflate_ctx.active) {
                        /* Decompress single-frame message. */
                        void*  dec     = NULL;
                        size_t dec_len = 0;
                        if (ws_deflate_decompress(&conn->deflate_ctx,
                                                  payload, (size_t)fh.payload_len,
                                                  &dec, &dec_len,
                                                  conn->max_msg_size) != 0) {
                            conn->close_code = 1009;
                            return -1;
                        }
                        msg_data = dec;
                        msg_len  = dec_len;
                    } else {
                        void* copy = malloc(fh.payload_len ? (size_t)fh.payload_len : 1);
                        if (!copy) {
                            return -1;
                        }
                        if (fh.payload_len) {
                            memcpy(copy, payload, (size_t)fh.payload_len);
                        }
                        msg_data = copy;
                        msg_len  = (size_t)fh.payload_len;
                    }

                    if (fh.opcode == 0x1) {
                        if (ws_utf8_validate(msg_data, msg_len) != 0) {
                            free(msg_data);
                            conn->close_code = 1007;
                            return -1;
                        }
                    }
                    msg->opcode = (xylem_ws_opcode_t)fh.opcode;
                    msg->data   = msg_data;
                    msg->len    = msg_len;
                    return 0;
                } else {
                    conn->frag_active     = true;
                    conn->frag_opcode     = fh.opcode;
                    conn->frag_compressed = fh.rsv1;
                    conn->frag_len        = 0;
                    if (_ws_frag_append(conn, payload, (size_t)fh.payload_len) != 0) {
                        conn->close_code = 1009;
                        return -1;
                    }
                }
            }
        }

        if (_ws_ensure_recv_buf(conn, conn->recv_len + 4096) != 0) {
            return -1;
        }
        int n = conn->transport.read(conn->transport.conn,
                                     conn->recv_buf + conn->recv_len,
                                     (int)(conn->recv_cap - conn->recv_len));
        if (n <= 0) {
            conn->close_code = 1006;
            return -1;
        }
        conn->recv_len += (size_t)n;
    }
}


int xylem_ws_close(xylem_ws_conn_t* conn, uint16_t code,
                   const char* reason, size_t reason_len) {
    if (!conn) {
        return -1;
    }

    if (!conn->close_sent) {
        if (code && ws_frame_close_validate_send(code) != 0) {
            code = 1000;
        }
        _ws_send_close_frame(conn, code ? code : 1000, reason, reason_len);
        conn->close_sent = true;
    }

    if (!conn->close_received) {
        conn->transport.set_rd_deadline(conn->transport.conn,
                                        conn->close_timeout_ms);
        for (;;) {
            if (_ws_ensure_recv_buf(conn, conn->recv_len + 4096) != 0) {
                break;
            }
            int n = conn->transport.read(conn->transport.conn,
                                         conn->recv_buf + conn->recv_len,
                                         (int)(conn->recv_cap - conn->recv_len));
            if (n <= 0) {
                break;
            }
            conn->recv_len += (size_t)n;

            ws_frame_header_t fh;
            size_t scan = 0;
            while (scan < conn->recv_len) {
                int rc = ws_frame_decode_header(conn->recv_buf + scan,
                                               conn->recv_len - scan, &fh);
                if (rc != 0) {
                    break;
                }
                size_t ftotal = fh.header_size + (size_t)fh.payload_len;
                if (scan + ftotal > conn->recv_len) {
                    break;
                }
                if (fh.opcode == 0x8) {
                    conn->close_received = true;
                    goto close_done;
                }
                scan += ftotal;
            }
        }
    }

close_done:
    conn->transport.close(conn->transport.conn);
    if (!conn->_standalone) {
        ws_conn_free(conn);
    }
    return 0;
}


xylem_ws_conn_t* ws_accept_impl(struct xylem_http_res_s* res,
                                 struct xylem_http_req_s* req,
                                 const xylem_ws_opts_t* opts) {
    if (!res || !req) {
        return NULL;
    }

    const char* ws_key = xylem_http_req_header(req, "Sec-WebSocket-Key");
    const char* ws_ver = xylem_http_req_header(req, "Sec-WebSocket-Version");
    if (!ws_key || !ws_ver || strcmp(ws_ver, "13") != 0) {
        return NULL;
    }

    char accept_val[29];
    if (ws_handshake_compute_accept(ws_key, accept_val, sizeof(accept_val)) != 0) {
        return NULL;
    }

    xylem_http_res_set_header(res, "Sec-WebSocket-Accept", accept_val);

    /* Negotiate permessage-deflate if server opts request it. */
    bool             deflate_agreed = false;
    ws_deflate_offer_t deflate_offer = {0};

    if (opts && opts->permessage_deflate) {
        const char* ext_hdr = xylem_http_req_header(req, "Sec-WebSocket-Extensions");
        if (ext_hdr && ws_deflate_parse_offer(ext_hdr, &deflate_offer) == 0) {
            /* If server does not want context takeover, force it. */
            if (!opts->deflate_context_takeover) {
                deflate_offer.server_no_context_takeover = true;
                deflate_offer.client_no_context_takeover = true;
            }
            char ext_resp[128];
            if (ws_deflate_build_server_accept(&deflate_offer, ext_resp,
                                               sizeof(ext_resp)) == 0) {
                xylem_http_res_set_header(res, "Sec-WebSocket-Extensions", ext_resp);
                deflate_agreed = true;
            }
        }
    }

    void* transport_ptr = NULL;
    if (xylem_http_res_upgrade(res, &transport_ptr) != 0) {
        return NULL;
    }

    http_transport_t* tp = (http_transport_t*)transport_ptr;
    xylem_ws_conn_t* conn = ws_conn_create(*tp, false, opts);
    if (!conn) {
        return NULL;
    }

    if (deflate_agreed) {
        bool no_takeover = deflate_offer.server_no_context_takeover;
        if (ws_deflate_init(&conn->deflate_ctx, no_takeover) != 0) {
            ws_conn_free(conn);
            return NULL;
        }
    }

    return conn;
}


xylem_ws_conn_t* ws_dial_impl(http_transport_t transport,
                               const char* host, uint16_t port,
                               const char* path,
                               const xylem_ws_opts_t* opts) {
    char key[25];
    if (ws_handshake_gen_key(key, sizeof(key)) != 0) {
        transport.close(transport.conn);
        return NULL;
    }

    /* Build extension offer if deflate is requested. */
    char   ext_offer[128];
    char*  ext_ptr       = NULL;
    bool   deflate_wanted = (opts && opts->permessage_deflate);

    if (deflate_wanted) {
        bool ctx_takeover = opts->deflate_context_takeover;
        if (ws_deflate_build_client_offer(ctx_takeover, ext_offer,
                                          sizeof(ext_offer)) == 0) {
            ext_ptr = ext_offer;
        }
    }

    size_t req_len;
    char*  req = ws_handshake_build_request_ext(host, port, path, key,
                                                ext_ptr, &req_len);
    if (!req) {
        transport.close(transport.conn);
        return NULL;
    }

    uint64_t hs_timeout = (opts && opts->handshake_timeout_ms)
                          ? opts->handshake_timeout_ms
                          : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    transport.set_wr_deadline(transport.conn, hs_timeout);
    int n = transport.write(transport.conn, req, (int)req_len);
    free(req);
    if (n < 0) {
        transport.close(transport.conn);
        return NULL;
    }
    transport.set_wr_deadline(transport.conn, 0);

    transport.set_rd_deadline(transport.conn, hs_timeout);
    char resp_buf[1024];
    size_t resp_len = 0;

    while (resp_len < sizeof(resp_buf) - 1) {
        int r = transport.read(transport.conn,
                               resp_buf + resp_len,
                               (int)(sizeof(resp_buf) - 1 - resp_len));
        if (r <= 0) {
            transport.close(transport.conn);
            return NULL;
        }
        resp_len += (size_t)r;
        resp_buf[resp_len] = '\0';

        if (strstr(resp_buf, "\r\n\r\n")) {
            break;
        }
    }
    transport.set_rd_deadline(transport.conn, 0);

    if (strncmp(resp_buf, "HTTP/1.1 101", 12) != 0) {
        transport.close(transport.conn);
        return NULL;
    }

    const char* acc = strstr(resp_buf, "Sec-WebSocket-Accept: ");
    if (!acc) {
        acc = strstr(resp_buf, "sec-websocket-accept: ");
    }
    if (!acc) {
        transport.close(transport.conn);
        return NULL;
    }
    acc += strlen("Sec-WebSocket-Accept: ");
    const char* acc_end = strstr(acc, "\r\n");
    if (!acc_end) {
        transport.close(transport.conn);
        return NULL;
    }
    char accept_got[64];
    size_t acc_len = (size_t)(acc_end - acc);
    if (acc_len >= sizeof(accept_got)) {
        transport.close(transport.conn);
        return NULL;
    }
    memcpy(accept_got, acc, acc_len);
    accept_got[acc_len] = '\0';

    if (ws_handshake_validate_accept(key, accept_got) != 0) {
        transport.close(transport.conn);
        return NULL;
    }

    /* Check if server accepted permessage-deflate. */
    bool deflate_accepted = false;
    bool no_context_takeover = false;

    if (deflate_wanted) {
        const char* ext_resp = strstr(resp_buf, "Sec-WebSocket-Extensions: ");
        if (!ext_resp) {
            ext_resp = strstr(resp_buf, "sec-websocket-extensions: ");
        }
        if (ext_resp) {
            ext_resp += strlen("Sec-WebSocket-Extensions: ");
            const char* ext_end = strstr(ext_resp, "\r\n");
            if (ext_end) {
                /* Temporarily null-terminate for parsing. */
                size_t ext_len = (size_t)(ext_end - ext_resp);
                char   ext_val[256];
                if (ext_len < sizeof(ext_val)) {
                    memcpy(ext_val, ext_resp, ext_len);
                    ext_val[ext_len] = '\0';

                    ws_deflate_offer_t server_offer = {0};
                    if (ws_deflate_parse_offer(ext_val, &server_offer) == 0) {
                        deflate_accepted = true;
                        no_context_takeover =
                            server_offer.server_no_context_takeover ||
                            !(opts->deflate_context_takeover);
                    }
                }
            }
        }
    }

    xylem_ws_conn_t* conn = ws_conn_create(transport, true, opts);
    if (!conn) {
        transport.close(transport.conn);
        return NULL;
    }

    if (deflate_accepted) {
        if (ws_deflate_init(&conn->deflate_ctx, no_context_takeover) != 0) {
            ws_conn_free(conn);
            transport.close(transport.conn);
            return NULL;
        }
    }

    /* Move any leftover data after headers into recv_buf */
    const char* body_start = strstr(resp_buf, "\r\n\r\n") + 4;
    size_t leftover = resp_len - (size_t)(body_start - resp_buf);
    if (leftover > 0) {
        memcpy(conn->recv_buf, body_start, leftover);
        conn->recv_len = leftover;
    }

    return conn;
}


void xylem_ws_msg_free(xylem_ws_msg_t* msg) {
    if (msg && msg->data) {
        free(msg->data);
        msg->data = NULL;
        msg->len = 0;
    }
}

uint16_t xylem_ws_close_code(xylem_ws_conn_t* conn) {
    return conn ? conn->close_code : 0;
}

void* xylem_ws_get_userdata(xylem_ws_conn_t* conn) {
    return conn ? conn->userdata : NULL;
}

void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud) {
    if (conn) {
        conn->userdata = ud;
    }
}
