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

#include <stddef.h>
#include <stdint.h>

#ifndef XYLEM_ADDR_MAXHOST
#define XYLEM_ADDR_MAXHOST 46
#endif

typedef struct xylem_rudp_conn_s   xylem_rudp_conn_t;
typedef struct xylem_rudp_server_s xylem_rudp_server_t;

typedef struct xylem_rudp_handler_s {
    void (*on_connect)(xylem_rudp_conn_t* rudp);           /*< Handshake completed (client). */
    void (*on_accept)(xylem_rudp_server_t* server,
                      xylem_rudp_conn_t* rudp);             /*< New session accepted (server). */
    void (*on_read)(xylem_rudp_conn_t* rudp,
                    void* data, size_t len);                /*< Reliable message received. */
    void (*on_close)(xylem_rudp_conn_t* rudp,
                     int err, const char* errmsg);          /*< Closed: 0 = normal, -1 = internal error, >0 = platform errno. */
} xylem_rudp_handler_t;

typedef struct xylem_rudp_opts_s {
    int32_t        mtu;           /*< MTU size, 0 for default (1400). */
    uint64_t       timeout_ms;    /*< Dead-link timeout in ms, 0 to disable. */
    uint64_t       handshake_ms;  /*< Handshake timeout in ms, 0 for default (5000). */
    int32_t        fec_data;      /*< FEC data shards, 0 to disable FEC. */
    int32_t        fec_parity;    /*< FEC parity shards, 0 to disable FEC. */
    const uint8_t* aes_key;       /*< 32-byte AES-256 key, NULL to disable encryption. */
} xylem_rudp_opts_t;

/**
 * @brief Initiate a reliable UDP connection.
 *
 * @param loop     Event loop.
 * @param host     Target address string.
 * @param port     Target port.
 * @param handler  Event callback set.
 * @param opts     RUDP options, NULL for defaults.
 *
 * @return RUDP handle, or NULL on failure.
 */
extern xylem_rudp_conn_t* xylem_rudp_dial(const char* host,
                                          uint16_t port,
                                          xylem_rudp_handler_t* handler,
                                          xylem_rudp_opts_t* opts);

/**
 * @brief Send data over a reliable UDP connection.
 *
 * @param rudp  RUDP handle.
 * @param data  Data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_rudp_send(xylem_rudp_conn_t* rudp,
                           const void* data, size_t len);

/**
 * @brief Close a reliable UDP connection.
 *
 * @param rudp  RUDP handle.
 */
extern void xylem_rudp_close(xylem_rudp_conn_t* rudp);

/**
 * @brief Increment the reference count of a RUDP session.
 *
 * @param rudp  RUDP handle.
 */
extern void xylem_rudp_conn_ref(xylem_rudp_conn_t* rudp);

/**
 * @brief Decrement the reference count of a RUDP session.
 *
 * @param rudp  RUDP handle.
 */
extern void xylem_rudp_conn_unref(xylem_rudp_conn_t* rudp);

/**
 * @brief Get the peer address of a connection.
 *
 * @param rudp  RUDP handle.
 * @param host  Output buffer, must be at least XYLEM_ADDR_MAXHOST bytes.
 * @param port  Output port number.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_rudp_remote_addr(xylem_rudp_conn_t* rudp,
                                  char host[XYLEM_ADDR_MAXHOST],
                                  uint16_t* port);

/**
 * @brief Get user data attached to a connection.
 *
 * @param rudp  RUDP handle.
 *
 * @return User data pointer.
 */
extern void* xylem_rudp_get_userdata(xylem_rudp_conn_t* rudp);

/**
 * @brief Set user data on a connection.
 *
 * @param rudp  RUDP handle.
 * @param ud    User data pointer.
 */
extern void xylem_rudp_set_userdata(xylem_rudp_conn_t* rudp, void* ud);

/**
 * @brief Create a reliable UDP server and start listening.
 *
 * @param loop     Event loop.
 * @param host     Bind address string.
 * @param port     Bind port.
 * @param handler  Event callback set.
 * @param opts     RUDP options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_rudp_server_t* xylem_rudp_listen(const char* host,
                                              uint16_t port,
                                              xylem_rudp_handler_t* handler,
                                              xylem_rudp_opts_t* opts);

/**
 * @brief Close a reliable UDP server.
 *
 * @param server  Server handle.
 */
extern void xylem_rudp_close_server(xylem_rudp_server_t* server);

/**
 * @brief Get user data attached to a server.
 *
 * @param server  Server handle.
 *
 * @return User data pointer.
 */
extern void* xylem_rudp_server_get_userdata(xylem_rudp_server_t* server);

/**
 * @brief Set user data on a server.
 *
 * @param server  Server handle.
 * @param ud      User data pointer.
 */
extern void xylem_rudp_server_set_userdata(xylem_rudp_server_t* server,
                                           void* ud);
