# WebSocket Server

`#include <xylem/ws/xylem-ws-server.h>`

异步 WebSocket 服务端，支持 ws:// 和 wss://（TLS）连接。

---

## 类型

### xylem_ws_srv_cfg_t {#xylem_ws_srv_cfg_t}

WebSocket 服务器配置。

```c
typedef struct xylem_ws_srv_cfg_s {
    const char*         host;       /* 绑定地址（如 "0.0.0.0"） */
    uint16_t            port;       /* 绑定端口 */
    xylem_ws_handler_t* handler;    /* 事件回调集合 */
    xylem_ws_opts_t*    opts;       /* 连接选项，NULL 使用默认值 */
    const char*         tls_cert;   /* PEM 证书路径，NULL 为 ws:// */
    const char*         tls_key;    /* PEM 密钥路径，NULL 为 ws:// */
} xylem_ws_srv_cfg_t;
```

| 字段 | 说明 |
|------|------|
| `host` | 绑定地址（如 `"0.0.0.0"`） |
| `port` | 绑定端口 |
| `handler` | 事件回调集合，见 [`xylem_ws_handler_t`](ws-common.md#xylem_ws_handler_t) |
| `opts` | 连接选项，`NULL` 使用默认值，见 [`xylem_ws_opts_t`](ws-common.md#xylem_ws_opts_t) |
| `tls_cert` | PEM 证书路径。设置后启用 `wss://`（TLS），`NULL` 为 `ws://` |
| `tls_key` | PEM 密钥路径。设置后启用 `wss://`（TLS），`NULL` 为 `ws://` |

---

## 函数

### xylem_ws_listen {#xylem_ws_listen}

```c
xylem_ws_server_t* xylem_ws_listen(xylem_loop_t* loop,
                                   const xylem_ws_srv_cfg_t* cfg);
```

创建 WebSocket 服务器并开始监听。绑定到 `cfg` 中指定的地址。当 `tls_cert` 和 `tls_key` 设置时，服务器接受 `wss://` TLS 连接；否则接受 `ws://` TCP 连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `cfg` | `const` [`xylem_ws_srv_cfg_t*`](#xylem_ws_srv_cfg_t) | 服务器配置 |

**返回值：** 服务器句柄，失败返回 `NULL`。

---

### xylem_ws_close_server {#xylem_ws_close_server}

```c
void xylem_ws_close_server(xylem_ws_server_t* server);
```

关闭 WebSocket 服务器。停止接受新连接并释放服务器句柄。已有连接不受影响，必须单独关闭。

| 参数 | 类型 | 说明 |
|------|------|------|
| `server` | [`xylem_ws_server_t*`](ws-common.md#xylem_ws_server_t) | 服务器句柄 |
