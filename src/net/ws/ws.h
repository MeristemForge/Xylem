_Pragma("once")

#include "net/http/http.h"
#include "xylem/net/xylem-ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_DEFAULT_MAX_MSG_SIZE       (16 * 1024 * 1024)
#define WS_DEFAULT_FRAGMENT_THRESHOLD (16 * 1024)
#define WS_DEFAULT_HANDSHAKE_TIMEOUT  10000
#define WS_DEFAULT_CLOSE_TIMEOUT      5000
#define WS_RECV_BUF_INIT              4096

struct xylem_ws_conn_s {
    http_transport_t  transport;
    bool              is_client;
    bool              _standalone;
    uint8_t*          recv_buf;
    size_t            recv_len;
    size_t            recv_cap;
    uint8_t*          frag_buf;
    size_t            frag_len;
    size_t            frag_cap;
    uint8_t           frag_opcode;
    bool              frag_active;
    size_t            max_msg_size;
    size_t            fragment_threshold;
    uint64_t          close_timeout_ms;
    uint16_t          close_code;
    bool              close_sent;
    bool              close_received;
    void*             userdata;
};

xylem_ws_conn_t* ws_accept_impl(struct xylem_http_res_s* res,
                                 struct xylem_http_req_s* req,
                                 const xylem_ws_opts_t* opts);

xylem_ws_conn_t* ws_conn_create(http_transport_t transport,
                                 bool is_client,
                                 const xylem_ws_opts_t* opts);

void ws_conn_free(xylem_ws_conn_t* conn);

xylem_ws_conn_t* ws_dial_impl(http_transport_t transport,
                               const char* host, uint16_t port,
                               const char* path,
                               const xylem_ws_opts_t* opts);
