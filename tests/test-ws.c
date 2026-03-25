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

#include "assert.h"
#include "xylem/ws/xylem-ws-common.h"
#include "xylem/ws/xylem-ws-client.h"
#include "xylem/ws/xylem-ws-server.h"

#include "ws/ws-frame.h"
#include "ws/ws-utf8.h"
#include "ws/ws-handshake.h"

#include <stdlib.h>
#include <string.h>

static void test_frame_encode_small(void) {
    uint8_t buf[14];
    uint8_t mask[4] = {0};
    size_t hdr_len = ws_frame_encode_header(buf, true, 0x1, false,
                                            mask, 10);
    ASSERT(hdr_len == 2);
    /* First byte: FIN=1, opcode=1 -> 0x81 */
    ASSERT(buf[0] == 0x81);
    /* Second byte: mask=0, len=10 */
    ASSERT(buf[1] == 10);
}

static void test_frame_encode_medium(void) {
    uint8_t buf[14];
    uint8_t mask[4] = {0};
    size_t hdr_len = ws_frame_encode_header(buf, true, 0x2, false,
                                            mask, 200);
    /* 2 base + 2 extended = 4 */
    ASSERT(hdr_len == 4);
    ASSERT((buf[1] & 0x7F) == 126);
    uint16_t ext_len = ((uint16_t)buf[2] << 8) | (uint16_t)buf[3];
    ASSERT(ext_len == 200);
}

static void test_frame_encode_large(void) {
    uint8_t buf[14];
    uint8_t mask[4] = {0};
    size_t hdr_len = ws_frame_encode_header(buf, true, 0x2, false,
                                            mask, 70000);
    /* 2 base + 8 extended = 10 */
    ASSERT(hdr_len == 10);
    ASSERT((buf[1] & 0x7F) == 127);
    uint64_t ext_len = 0;
    for (int i = 0; i < 8; i++) {
        ext_len = (ext_len << 8) | (uint64_t)buf[2 + i];
    }
    ASSERT(ext_len == 70000);
}

static void test_frame_decode_roundtrip(void) {
    uint8_t buf[14];
    uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    size_t hdr_len = ws_frame_encode_header(buf, true, 0x1, true,
                                            mask, 50);

    ws_frame_header_t hdr;
    int rc = ws_frame_decode_header(buf, hdr_len, &hdr);
    ASSERT(rc == 0);
    ASSERT(hdr.fin == true);
    ASSERT(hdr.opcode == 0x1);
    ASSERT(hdr.masked == true);
    ASSERT(hdr.payload_len == 50);
    ASSERT(hdr.mask_key[0] == 0xAA);
    ASSERT(hdr.mask_key[1] == 0xBB);
    ASSERT(hdr.mask_key[2] == 0xCC);
    ASSERT(hdr.mask_key[3] == 0xDD);
    ASSERT(hdr.header_size == hdr_len);
}

static void test_frame_decode_insufficient(void) {
    uint8_t buf[1] = {0x81};
    ws_frame_header_t hdr;
    int rc = ws_frame_decode_header(buf, 1, &hdr);
    ASSERT(rc == -1);
}

static void test_frame_decode_reserved_opcode(void) {
    /* Construct a frame with opcode 0x3 (reserved) */
    uint8_t buf[2] = {0x83, 0x00}; /* FIN=1, opcode=3, len=0 */
    ws_frame_header_t hdr;
    int rc = ws_frame_decode_header(buf, 2, &hdr);
    ASSERT(rc == -2);
}

static void test_frame_decode_control_too_long(void) {
    /* Ping frame (opcode 0x9) with 16-bit extended length = 200 */
    uint8_t buf[4] = {0x89, 126, 0x00, 0xC8};
    ws_frame_header_t hdr;
    int rc = ws_frame_decode_header(buf, 4, &hdr);
    ASSERT(rc == -2);
}

static void test_frame_decode_control_not_fin(void) {
    /* Ping frame with FIN=0 (0x09 instead of 0x89) */
    uint8_t buf[2] = {0x09, 0x00};
    ws_frame_header_t hdr;
    int rc = ws_frame_decode_header(buf, 2, &hdr);
    ASSERT(rc == -2);
}

static void test_frame_mask_involution(void) {
    uint8_t original[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    uint8_t data[5];
    memcpy(data, original, 5);

    uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};

    /* Mask once */
    ws_frame_apply_mask(data, 5, mask, 0);
    /* Data should differ from original */
    ASSERT(memcmp(data, original, 5) != 0);

    /* Mask again with same key restores original */
    ws_frame_apply_mask(data, 5, mask, 0);
    ASSERT(memcmp(data, original, 5) == 0);
}

static void test_close_validate_send_valid(void) {
    ASSERT(ws_frame_close_validate_send(1000) == 0);
    ASSERT(ws_frame_close_validate_send(1001) == 0);
    ASSERT(ws_frame_close_validate_send(1007) == 0);
    ASSERT(ws_frame_close_validate_send(3000) == 0);
    ASSERT(ws_frame_close_validate_send(4999) == 0);
}

static void test_close_validate_send_invalid(void) {
    ASSERT(ws_frame_close_validate_send(999) == -1);
    ASSERT(ws_frame_close_validate_send(1004) == -1);
    ASSERT(ws_frame_close_validate_send(1005) == -1);
    ASSERT(ws_frame_close_validate_send(1006) == -1);
    ASSERT(ws_frame_close_validate_send(1015) == -1);
    ASSERT(ws_frame_close_validate_send(2000) == -1);
}

static void test_close_validate_recv_valid(void) {
    ASSERT(ws_frame_close_validate_recv(1000) == 0);
    ASSERT(ws_frame_close_validate_recv(1001) == 0);
    ASSERT(ws_frame_close_validate_recv(1007) == 0);
    ASSERT(ws_frame_close_validate_recv(3000) == 0);
}

static void test_close_validate_recv_invalid(void) {
    ASSERT(ws_frame_close_validate_recv(0) == -1);
    ASSERT(ws_frame_close_validate_recv(999) == -1);
    ASSERT(ws_frame_close_validate_recv(1004) == -1);
    ASSERT(ws_frame_close_validate_recv(1015) == -1);
    ASSERT(ws_frame_close_validate_recv(2000) == -1);
}

static void test_close_encode_decode_roundtrip(void) {
    uint8_t payload[128];
    const char* reason = "going away";
    size_t reason_len = strlen(reason);

    int enc_len = ws_frame_close_encode(1001, reason, reason_len,
                                        payload, sizeof(payload));
    ASSERT(enc_len == (int)(2 + reason_len));

    uint16_t    code_out = 0;
    const char* reason_out = NULL;
    size_t      reason_out_len = 0;

    int rc = ws_frame_close_decode(payload, (size_t)enc_len,
                                   &code_out, &reason_out, &reason_out_len);
    ASSERT(rc == 0);
    ASSERT(code_out == 1001);
    ASSERT(reason_out_len == reason_len);
    ASSERT(memcmp(reason_out, reason, reason_len) == 0);
}

static void test_close_decode_empty(void) {
    uint16_t    code = 0;
    const char* reason = NULL;
    size_t      reason_len = 0;

    int rc = ws_frame_close_decode(NULL, 0, &code, &reason, &reason_len);
    ASSERT(rc == 0);
    ASSERT(code == 1005);
    ASSERT(reason == NULL);
    ASSERT(reason_len == 0);
}

static void test_close_decode_one_byte(void) {
    uint8_t data[1] = {0x03};
    uint16_t    code = 0;
    const char* reason = NULL;
    size_t      reason_len = 0;

    int rc = ws_frame_close_decode(data, 1, &code, &reason, &reason_len);
    ASSERT(rc == -1);
}

static void test_utf8_valid_ascii(void) {
    const uint8_t data[] = "hello";
    ASSERT(ws_utf8_validate(data, 5) == 0);
}

static void test_utf8_valid_multibyte(void) {
    /* U+4F60 U+597D = "ni hao" in UTF-8: E4 BD A0 E5 A5 BD */
    const uint8_t data[] = {0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD};
    ASSERT(ws_utf8_validate(data, sizeof(data)) == 0);
}

static void test_utf8_invalid_continuation(void) {
    /* 0x80 alone is a continuation byte without a lead byte */
    const uint8_t data[] = {0x80};
    ASSERT(ws_utf8_validate(data, 1) == -1);
}

static void test_utf8_overlong_2byte(void) {
    /* 0xC0 0x80 encodes U+0000 in 2 bytes (overlong) */
    const uint8_t data[] = {0xC0, 0x80};
    ASSERT(ws_utf8_validate(data, 2) == -1);
}

static void test_utf8_surrogate(void) {
    /* 0xED 0xA0 0x80 encodes U+D800 (surrogate half) */
    const uint8_t data[] = {0xED, 0xA0, 0x80};
    ASSERT(ws_utf8_validate(data, 3) == -1);
}

static void test_utf8_beyond_max(void) {
    /* 0xF4 0x90 0x80 0x80 encodes U+110000 (> U+10FFFF) */
    const uint8_t data[] = {0xF4, 0x90, 0x80, 0x80};
    ASSERT(ws_utf8_validate(data, 4) == -1);
}

static void test_utf8_truncated(void) {
    /* 0xE4 alone is a 3-byte lead with no continuation bytes */
    const uint8_t data[] = {0xE4};
    ASSERT(ws_utf8_validate(data, 1) == -1);
}

static void test_utf8_empty(void) {
    ASSERT(ws_utf8_validate(NULL, 0) == 0);
}

static void test_handshake_gen_key(void) {
    char key[32];
    int rc = ws_handshake_gen_key(key, sizeof(key));
    ASSERT(rc == 0);
    /* Base64 of 16 bytes = 24 characters */
    ASSERT(strlen(key) == 24);
}

static void test_handshake_accept_roundtrip(void) {
    char key[32];
    ASSERT(ws_handshake_gen_key(key, sizeof(key)) == 0);

    char accept[32];
    ASSERT(ws_handshake_compute_accept(key, accept, sizeof(accept)) == 0);

    ASSERT(ws_handshake_validate_accept(key, accept) == 0);
}

static void test_handshake_accept_mismatch(void) {
    char key[32];
    ASSERT(ws_handshake_gen_key(key, sizeof(key)) == 0);

    ASSERT(ws_handshake_validate_accept(key, "bogus_accept_value======") == -1);
}

static void test_handshake_build_request(void) {
    char key[32];
    ASSERT(ws_handshake_gen_key(key, sizeof(key)) == 0);

    size_t len = 0;
    char* req = ws_handshake_build_request("example.com", 9090,
                                           "/ws", key, &len);
    ASSERT(req != NULL);
    ASSERT(len > 0);
    ASSERT(len == strlen(req));

    /* Verify required headers are present */
    ASSERT(strstr(req, "GET /ws HTTP/1.1\r\n") != NULL);
    ASSERT(strstr(req, "Upgrade: websocket\r\n") != NULL);
    ASSERT(strstr(req, "Connection: Upgrade\r\n") != NULL);
    ASSERT(strstr(req, "Sec-WebSocket-Key: ") != NULL);
    ASSERT(strstr(req, "Sec-WebSocket-Version: 13\r\n") != NULL);
    ASSERT(strstr(req, "Host: example.com:9090\r\n") != NULL);

    free(req);
}

static void test_handshake_build_response(void) {
    char key[32];
    ASSERT(ws_handshake_gen_key(key, sizeof(key)) == 0);

    char accept[32];
    ASSERT(ws_handshake_compute_accept(key, accept, sizeof(accept)) == 0);

    size_t len = 0;
    char* resp = ws_handshake_build_response(accept, &len);
    ASSERT(resp != NULL);
    ASSERT(len > 0);
    ASSERT(len == strlen(resp));

    ASSERT(strstr(resp, "HTTP/1.1 101 Switching Protocols\r\n") != NULL);
    ASSERT(strstr(resp, "Upgrade: websocket\r\n") != NULL);
    ASSERT(strstr(resp, "Connection: Upgrade\r\n") != NULL);
    ASSERT(strstr(resp, "Sec-WebSocket-Accept: ") != NULL);

    free(resp);
}

static void test_ws_dial_null_args(void) {
    xylem_ws_handler_t handler = {0};
    ASSERT(xylem_ws_dial(NULL, "ws://localhost", &handler, NULL) == NULL);
}

static void test_ws_send_null(void) {
    ASSERT(xylem_ws_send(NULL, XYLEM_WS_OPCODE_TEXT, "hi", 2) == -1);
}

static void test_ws_ping_null(void) {
    ASSERT(xylem_ws_ping(NULL, "p", 1) == -1);
}

static void test_ws_close_null(void) {
    ASSERT(xylem_ws_close(NULL, 1000, NULL, 0) == -1);
}

static void test_ws_userdata_null(void) {
    /* get/set with NULL conn must not crash */
    ASSERT(xylem_ws_get_userdata(NULL) == NULL);
    xylem_ws_set_userdata(NULL, NULL);
}

static void test_ws_listen_null(void) {
    xylem_ws_srv_cfg_t cfg = {0};
    ASSERT(xylem_ws_listen(NULL, &cfg) == NULL);
}

static void test_ws_close_server_null(void) {
    /* Must not crash */
    xylem_ws_close_server(NULL);
}

int main(void) {
    /* Frame encode/decode */
    test_frame_encode_small();
    test_frame_encode_medium();
    test_frame_encode_large();
    test_frame_decode_roundtrip();
    test_frame_decode_insufficient();
    test_frame_decode_reserved_opcode();
    test_frame_decode_control_too_long();
    test_frame_decode_control_not_fin();
    test_frame_mask_involution();

    /* Close frame */
    test_close_validate_send_valid();
    test_close_validate_send_invalid();
    test_close_validate_recv_valid();
    test_close_validate_recv_invalid();
    test_close_encode_decode_roundtrip();
    test_close_decode_empty();
    test_close_decode_one_byte();

    /* UTF-8 validation */
    test_utf8_valid_ascii();
    test_utf8_valid_multibyte();
    test_utf8_invalid_continuation();
    test_utf8_overlong_2byte();
    test_utf8_surrogate();
    test_utf8_beyond_max();
    test_utf8_truncated();
    test_utf8_empty();

    /* Handshake */
    test_handshake_gen_key();
    test_handshake_accept_roundtrip();
    test_handshake_accept_mismatch();
    test_handshake_build_request();
    test_handshake_build_response();

    /* Public API null guards */
    test_ws_dial_null_args();
    test_ws_send_null();
    test_ws_ping_null();
    test_ws_close_null();
    test_ws_userdata_null();
    test_ws_listen_null();
    test_ws_close_server_null();

    return 0;
}
