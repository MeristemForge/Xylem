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

#include "ws-common.h"
#include "ws-frame.h"
#include "ws-utf8.h"

#include "xylem/xylem-utils.h"

#include <stdlib.h>
#include <string.h>

/* ASCII lowercase table: maps A-Z to a-z, all others pass through. */
static const uint8_t _ws_lower[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
    26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,
    115,116,117,118,119,120,121,122,
    91,92,93,94,95,96,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,
    115,116,117,118,119,120,121,122,
    123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
    208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
};

bool ws_memeqi(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (_ws_lower[(uint8_t)a[i]] != _ws_lower[(uint8_t)b[i]]) {
            return false;
        }
    }
    return true;
}

int ws_conn_recv_buf_grow(xylem_ws_conn_t* conn, size_t needed) {
    if (conn->recv_cap >= needed) {
        return 0;
    }
    size_t new_cap = conn->recv_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    uint8_t* tmp = realloc(conn->recv_buf, new_cap);
    if (!tmp) {
        return -1;
    }
    conn->recv_buf = tmp;
    conn->recv_cap = new_cap;
    return 0;
}

int ws_conn_frag_buf_append(xylem_ws_conn_t* conn,
                            const uint8_t* data, size_t len) {
    size_t needed = conn->frag_len + len;
    if (conn->frag_cap < needed) {
        size_t new_cap = conn->frag_cap ? conn->frag_cap : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t* tmp = realloc(conn->frag_buf, new_cap);
        if (!tmp) {
            return -1;
        }
        conn->frag_buf = tmp;
        conn->frag_cap = new_cap;
    }
    memcpy(conn->frag_buf + conn->frag_len, data, len);
    conn->frag_len += len;
    return 0;
}

int ws_conn_send_frame(xylem_ws_conn_t* conn, bool fin,
                       uint8_t opcode, const void* data, size_t len) {
    bool masked = conn->is_client;
    uint8_t mask_key[4] = {0};

    if (masked) {
        for (int i = 0; i < 4; i++) {
            mask_key[i] = (uint8_t)xylem_utils_getprng(0, 255);
        }
    }

    uint8_t hdr_buf[WS_MAX_HEADER_SIZE];
    size_t hdr_len = ws_frame_encode_header(hdr_buf, fin, opcode,
                                            masked, mask_key, (uint64_t)len);

    size_t total = hdr_len + len;

    /* Use stack buffer for small frames to avoid malloc overhead */
    uint8_t stack_buf[256];
    uint8_t* frame;
    bool heap = (total > sizeof(stack_buf));

    if (heap) {
        frame = malloc(total);
        if (!frame) {
            return -1;
        }
    } else {
        frame = stack_buf;
    }

    memcpy(frame, hdr_buf, hdr_len);
    if (len > 0 && data) {
        memcpy(frame + hdr_len, data, len);
        if (masked) {
            ws_frame_apply_mask(frame + hdr_len, len, mask_key, 0);
        }
    }

    int rc = conn->vt->send(conn->transport, frame, total);
    if (heap) {
        free(frame);
    }
    return rc;
}

void ws_conn_send_close_frame(xylem_ws_conn_t* conn, uint16_t code,
                              const char* reason, size_t reason_len) {
    uint8_t payload[125];
    int plen = ws_frame_close_encode(code, reason, reason_len,
                                       payload, sizeof(payload));
    if (plen < 0) {
        plen = 0;
    }
    ws_conn_send_frame(conn, true, WS_OPCODE_CLOSE,
                       payload, (size_t)plen);
    conn->close_sent = true;
}

void ws_conn_destroy(xylem_ws_conn_t* conn) {
    if (!conn) {
        return;
    }
    free(conn->recv_buf);
    conn->recv_buf = NULL;
    conn->recv_len = 0;
    conn->recv_cap = 0;

    free(conn->frag_buf);
    conn->frag_buf = NULL;
    conn->frag_len = 0;
    conn->frag_cap = 0;

    free(conn->host);
    conn->host = NULL;
    free(conn->path);
    conn->path = NULL;

    free(conn);
}

xylem_ws_conn_t* ws_conn_create(xylem_loop_t* loop,
                                const xylem_ws_opts_t* opts) {
    xylem_ws_conn_t* conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    conn->loop = loop;

    /* Apply options with defaults */
    conn->max_message_size   = WS_DEFAULT_MAX_MESSAGE_SIZE;
    conn->fragment_threshold = WS_DEFAULT_FRAGMENT_THRESHOLD;
    conn->close_timeout_ms   = WS_DEFAULT_CLOSE_TIMEOUT;

    if (opts) {
        if (opts->max_message_size > 0) {
            conn->max_message_size = opts->max_message_size;
        }
        if (opts->fragment_threshold > 0) {
            conn->fragment_threshold = opts->fragment_threshold;
        }
        if (opts->close_timeout_ms > 0) {
            conn->close_timeout_ms = opts->close_timeout_ms;
        }
    }

    /* Allocate receive buffer */
    conn->recv_buf = malloc(WS_INITIAL_RECV_CAP);
    if (!conn->recv_buf) {
        free(conn);
        return NULL;
    }
    conn->recv_cap = WS_INITIAL_RECV_CAP;

    /* Initialize timers */
    xylem_loop_init_timer(loop, &conn->handshake_timer);
    xylem_loop_init_timer(loop, &conn->close_timer);

    return conn;
}

void ws_conn_fire_close(xylem_ws_conn_t* conn, uint16_t code,
                        const char* reason, size_t reason_len) {
    if (conn->state == XYLEM_WS_STATE_CLOSED) {
        return;
    }
    conn->state = XYLEM_WS_STATE_CLOSED;

    /* Stop any active timers */
    if (conn->handshake_timer.active) {
        xylem_loop_stop_timer(&conn->handshake_timer);
    }
    if (conn->close_timer.active) {
        xylem_loop_stop_timer(&conn->close_timer);
    }

    if (conn->handler && conn->handler->on_close) {
        conn->handler->on_close(conn, code, reason, reason_len);
    }

    ws_conn_destroy(conn);
}

void ws_conn_close_timeout_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_ws_conn_t* conn = (xylem_ws_conn_t*)((char*)timer -
        offsetof(xylem_ws_conn_t, close_timer));
    conn->vt->close_conn(conn->transport);
}

void ws_conn_protocol_error(xylem_ws_conn_t* conn, uint16_t code) {
    if (conn->state == XYLEM_WS_STATE_OPEN) {
        conn->state = XYLEM_WS_STATE_CLOSING;
        conn->close_code = code;
        ws_conn_send_close_frame(conn, code, NULL, 0);
        xylem_loop_start_timer(&conn->close_timer,
                               ws_conn_close_timeout_cb,
                               conn->close_timeout_ms, 0);
    } else if (conn->state == XYLEM_WS_STATE_CLOSING) {
        conn->vt->close_conn(conn->transport);
    }
}

void ws_conn_handle_close_frame(xylem_ws_conn_t* conn,
                                const uint8_t* payload, size_t len) {
    uint16_t    code       = 0;
    const char* reason     = NULL;
    size_t      reason_len = 0;

    int rc = ws_frame_close_decode(payload, len, &code, &reason, &reason_len);
    if (rc == -1) {
        /* 1-byte payload -> protocol error */
        ws_conn_protocol_error(conn, 1002);
        return;
    }
    if (rc == -2) {
        /* Reserved status code */
        ws_conn_protocol_error(conn, 1002);
        return;
    }

    /* Validate reason UTF-8 if present */
    if (reason_len > 0) {
        if (ws_utf8_validate((const uint8_t*)reason, reason_len) != 0) {
            ws_conn_protocol_error(conn, 1007);
            return;
        }
    }

    conn->close_received = true;

    if (conn->state == XYLEM_WS_STATE_OPEN) {
        /* Peer initiated close -- echo the close frame, then shut down */
        conn->state = XYLEM_WS_STATE_CLOSING;
        conn->close_code = code;
        ws_conn_send_close_frame(conn, code, reason, reason_len);
        conn->vt->close_conn(conn->transport);
    } else if (conn->state == XYLEM_WS_STATE_CLOSING) {
        /* We initiated close, peer responded -- shut down */
        if (conn->close_timer.active) {
            xylem_loop_stop_timer(&conn->close_timer);
        }
        conn->vt->close_conn(conn->transport);
    }
}

void ws_conn_deliver_message(xylem_ws_conn_t* conn, uint8_t opcode,
                             const uint8_t* data, size_t len) {
    /* UTF-8 validation for text messages */
    if (opcode == XYLEM_WS_OPCODE_TEXT) {
        if (ws_utf8_validate(data, len) != 0) {
            ws_conn_protocol_error(conn, 1007);
            return;
        }
    }

    if (conn->handler && conn->handler->on_message) {
        conn->handler->on_message(conn, (xylem_ws_opcode_t)opcode,
                                  data, len);
    }
}

void ws_conn_process_recv(xylem_ws_conn_t* conn) {
    size_t consumed = 0;

    while (consumed < conn->recv_len && conn->state != XYLEM_WS_STATE_CLOSED) {
        uint8_t* buf = conn->recv_buf + consumed;
        size_t   avail = conn->recv_len - consumed;

        ws_frame_header_t hdr;
        int rc = ws_frame_decode_header(buf, avail, &hdr);

        if (rc == -1) {
            break;
        }
        if (rc == -2) {
            ws_conn_protocol_error(conn, 1002);
            return;
        }

        size_t frame_total = hdr.header_size + (size_t)hdr.payload_len;
        if (avail < frame_total) {
            break;
        }

        uint8_t* payload = buf + hdr.header_size;
        size_t   plen    = (size_t)hdr.payload_len;

        if (conn->is_client && hdr.masked) {
            ws_conn_protocol_error(conn, 1002);
            return;
        }
        if (!conn->is_client && !hdr.masked) {
            ws_conn_protocol_error(conn, 1002);
            return;
        }

        if (hdr.masked) {
            ws_frame_apply_mask(payload, plen, hdr.mask_key, 0);
        }

        bool is_control = (hdr.opcode >= 0x8);

        if (is_control) {
            if (hdr.opcode == WS_OPCODE_CLOSE) {
                ws_conn_handle_close_frame(conn, payload, plen);
            } else if (hdr.opcode == WS_OPCODE_PING) {
                ws_conn_send_frame(conn, true, WS_OPCODE_PONG,
                                   payload, plen);
                if (conn->handler && conn->handler->on_ping) {
                    conn->handler->on_ping(conn, payload, plen);
                }
            } else if (hdr.opcode == WS_OPCODE_PONG) {
                if (conn->handler && conn->handler->on_pong) {
                    conn->handler->on_pong(conn, payload, plen);
                }
            }
        } else {
            if (hdr.opcode != 0x0) {
                if (conn->frag_active) {
                    ws_conn_protocol_error(conn, 1002);
                    return;
                }

                if (hdr.fin) {
                    if (conn->max_message_size > 0 &&
                        plen > conn->max_message_size) {
                        ws_conn_protocol_error(conn, 1009);
                        return;
                    }
                    ws_conn_deliver_message(conn, hdr.opcode, payload, plen);
                } else {
                    if (conn->max_message_size > 0 &&
                        plen > conn->max_message_size) {
                        ws_conn_protocol_error(conn, 1009);
                        return;
                    }
                    conn->frag_active = true;
                    conn->frag_opcode = hdr.opcode;
                    conn->frag_len = 0;
                    if (ws_conn_frag_buf_append(conn, payload, plen) != 0) {
                        ws_conn_protocol_error(conn, 1009);
                        return;
                    }
                }
            } else {
                if (!conn->frag_active) {
                    ws_conn_protocol_error(conn, 1002);
                    return;
                }

                if (conn->max_message_size > 0 &&
                    (conn->frag_len + plen) > conn->max_message_size) {
                    conn->frag_active = false;
                    conn->frag_len = 0;
                    ws_conn_protocol_error(conn, 1009);
                    return;
                }

                if (ws_conn_frag_buf_append(conn, payload, plen) != 0) {
                    ws_conn_protocol_error(conn, 1009);
                    return;
                }

                if (hdr.fin) {
                    uint8_t opcode = conn->frag_opcode;
                    conn->frag_active = false;
                    ws_conn_deliver_message(conn, opcode,
                                            conn->frag_buf, conn->frag_len);
                    conn->frag_len = 0;
                }
            }
        }

        consumed += frame_total;
    }

    /* Compact recv_buf once after processing all complete frames */
    if (consumed > 0) {
        size_t remaining = conn->recv_len - consumed;
        if (remaining > 0) {
            memmove(conn->recv_buf, conn->recv_buf + consumed, remaining);
        }
        conn->recv_len = remaining;
    }
}
