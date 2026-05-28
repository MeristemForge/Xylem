# WebSocket 协程化设计

## 概述

将现有基于回调的 WS/WSS 实现改造为协程化 API，与 HTTP/HTTPS 协程模型保持一致。删除旧回调实现，新代码直接放 `src/net/ws/`。

## 设计决策

| 决策 | 选项 | 结论 |
|------|------|------|
| 服务端模型 | HTTP upgrade / standalone / 两者 | 两者都提供 |
| WS/WSS 拆分 | 统一 / 分开头文件 | 分开（匹配 HTTP/HTTPS 模式） |
| 连接类型 | 统一 / 分开 | 统一 `xylem_ws_conn_t` |
| 内部实现 | 重写 / 复用计算层 / 基于 http_transport_t | 基于 `http_transport_t` + 复用帧/握手/UTF-8 |
| 旧代码 | 保留 / 删除 | 删除回调实现 |

## 公共 API

### xylem-ws.h（Plain WS）

```c
_Pragma("once")

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_ws_conn_s     xylem_ws_conn_t;
typedef struct xylem_ws_listener_s xylem_ws_listener_t;

typedef enum {
    XYLEM_WS_TEXT   = 0x1,
    XYLEM_WS_BINARY = 0x2,
} xylem_ws_opcode_t;

typedef struct {
    xylem_ws_opcode_t opcode;
    void*             data;
    size_t            len;
} xylem_ws_msg_t;

typedef void (*xylem_ws_handler_fn_t)(xylem_ws_conn_t* conn, void* userdata);

typedef struct {
    size_t   max_msg_size;          /* 0 = 默认 16 MiB */
    size_t   fragment_threshold;    /* 0 = 默认 16 KiB */
    uint64_t handshake_timeout_ms;  /* 0 = 默认 10000 ms */
    uint64_t close_timeout_ms;      /* 0 = 默认 5000 ms */
} xylem_ws_opts_t;

/* --- 服务端：HTTP upgrade 路径 --- */

struct xylem_http_res_s;
struct xylem_http_req_s;

xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                  struct xylem_http_req_s* req,
                                  const xylem_ws_opts_t* opts);

/* --- 服务端：standalone listener --- */

xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                      xylem_ws_handler_fn_t handler,
                                      void* userdata,
                                      const xylem_ws_opts_t* opts);
void     xylem_ws_close_listener(xylem_ws_listener_t* listener);
uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener);

/* --- 客户端 --- */

xylem_ws_conn_t* xylem_ws_dial(const char* url,
                                const xylem_ws_opts_t* opts);

/* --- 连接操作 --- */

int  xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                   const void* data, size_t len);
int  xylem_ws_recv(xylem_ws_conn_t* conn, xylem_ws_msg_t* msg);
int  xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len);
int  xylem_ws_close(xylem_ws_conn_t* conn, uint16_t code,
                    const char* reason, size_t reason_len);
void xylem_ws_msg_free(xylem_ws_msg_t* msg);

/* --- 辅助 --- */

uint16_t xylem_ws_close_code(xylem_ws_conn_t* conn);
void*    xylem_ws_get_userdata(xylem_ws_conn_t* conn);
void     xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
```

### xylem-wss.h（WSS）

```c
_Pragma("once")

#include "xylem/net/xylem-ws.h"
#include "xylem/net/xylem-tls.h"

typedef struct xylem_wss_listener_s xylem_wss_listener_t;

/* --- 服务端：HTTPS upgrade 路径 --- */

xylem_ws_conn_t* xylem_wss_accept(struct xylem_http_res_s* res,
                                   struct xylem_http_req_s* req,
                                   const xylem_ws_opts_t* opts);

/* --- 服务端：standalone listener --- */

xylem_wss_listener_t* xylem_wss_listen(const char* host, uint16_t port,
                                        xylem_ws_handler_fn_t handler,
                                        void* userdata,
                                        xylem_tls_ctx_t* tls_ctx,
                                        const xylem_ws_opts_t* opts);
void     xylem_wss_close_listener(xylem_wss_listener_t* listener);
uint16_t xylem_wss_listener_port(xylem_wss_listener_t* listener);

/* --- 客户端 --- */

xylem_ws_conn_t* xylem_wss_dial(const char* url,
                                 xylem_tls_ctx_t* tls_ctx,
                                 const xylem_ws_opts_t* opts);
```

## 内部架构

### 文件布局

```
include/xylem/net/
    xylem-ws.h
    xylem-wss.h

src/net/ws/
    ws.h                # 内部 xylem_ws_conn_s 定义 + 内部函数声明
    ws.c                # 共享逻辑：recv 循环、send、close 握手、accept impl
    xylem-ws.c          # plain WS 入口：listen/dial（TCP transport）
    xylem-wss.c         # WSS 入口：listen/dial（TLS transport）
    ws-frame.h          # 保留：帧头编解码、mask
    ws-frame.c          # 保留
    ws-handshake.h      # 保留：key 生成、accept 计算、请求/响应构造
    ws-handshake.c      # 保留
    ws-utf8.h           # 保留：UTF-8 校验
    ws-utf8.c           # 保留
```

### 删除的文件

```
include/xylem/net/ws/            # 整个旧公共头目录
src/net/ws/ws-common.h           # 旧内部结构
src/net/ws/ws-common.c
src/net/ws/ws-transport.h        # 旧 transport vtable
src/net/ws/ws-transport-tcp.c
src/net/ws/ws-transport-tls.c
src/net/ws/ws-transport-tls-stub.c
src/net/ws/xylem-ws-client.c     # 旧客户端
src/net/ws/xylem-ws-server.c     # 旧服务端
```

### 核心内部结构（ws.h）

```c
#include "net/http/http.h"  /* http_transport_t */

typedef struct xylem_ws_conn_s {
    http_transport_t transport;
    bool             is_client;
    uint8_t*         recv_buf;
    size_t           recv_len;
    size_t           recv_cap;
    uint8_t*         frag_buf;
    size_t           frag_len;
    size_t           frag_cap;
    uint8_t          frag_opcode;
    bool             frag_active;
    size_t           max_msg_size;
    size_t           fragment_threshold;
    uint64_t         close_timeout_ms;
    uint16_t         close_code;
    bool             close_sent;
    bool             close_received;
    void*            userdata;
} xylem_ws_conn_t;

/* 内部共享实现 */
xylem_ws_conn_t* ws_accept_impl(struct xylem_http_res_s* res,
                                 struct xylem_http_req_s* req,
                                 const xylem_ws_opts_t* opts);
```

## 数据流

### 服务端：HTTP upgrade 路径

```
HTTP accept coroutine
  → on_upgrade handler 被调用（已在独立协程中）
    → xylem_ws_accept(res, req, opts)
      → 验证 Sec-WebSocket-Key / Version: 13
      → 构造 101 响应 + Sec-WebSocket-Accept
      → 调 xylem_http_res_upgrade(res, &transport) 发送 101 并分离连接
      → 分配 ws_conn_t，填入 transport（is_client=false）
      → 返回 conn
    → 用户循环 recv/send
    → xylem_ws_close(conn, ...)
```

### 服务端：standalone listen

```
xylem_ws_listen(host, port, handler, ud, opts)
  → xylem_http_listen(host, port, NULL, NULL, &srv_opts)
    → srv_opts.on_upgrade = _ws_upgrade_handler
  → _ws_upgrade_handler(res, req, ctx):
    → conn = ws_accept_impl(res, req, opts)
    → runtime_spawn(_ws_conn_coroutine, {conn, handler, ud})
  → _ws_conn_coroutine:
    → handler(conn, ud)
    → if (!conn->close_sent) xylem_ws_close(conn, 1000, NULL, 0)
    → else free(conn)  // close 已经释放过
```

### 客户端

```
xylem_ws_dial(url, opts)
  → 解析 ws://host:port/path
  → xylem_tcp_dial(host, port, NULL)  // 协程挂起
  → 构造 http_transport_t（TCP 函数指针）
  → 用 ws-handshake 生成 Sec-WebSocket-Key
  → transport->write 发送 HTTP Upgrade 请求
  → transport->read 读响应
  → 验证 101 + Sec-WebSocket-Accept
  → 分配 ws_conn_t（is_client=true）
  → 返回 conn
```

WSS 客户端同理，用 `xylem_tls_dial` + TLS transport 函数指针。

## Close 握手与错误处理

### xylem_ws_close 语义

- **主动关闭**（close_received=false）：发送 close 帧 → 设置 read deadline → 循环读直到收到对端 close 或超时 → 关闭 transport → 释放 conn
- **被动关闭**（close_received=true）：发送 close 帧 → 关闭 transport → 释放 conn
- **幂等**：close_sent=true 时直接关闭 transport + 释放

### xylem_ws_recv 返回值

| 返回 | 含义 |
|------|------|
| 0 | 成功，msg 已填充 |
| -1 | 连接关闭或错误 |

### xylem_ws_send 返回值

| 返回 | 含义 |
|------|------|
| 0 | 成功 |
| -1 | 失败（已关闭或 write 错误） |

### 控制帧处理（recv 内部）

| 收到 | 动作 |
|------|------|
| Ping | 自动回复 Pong，不暴露给用户 |
| Pong | 静默丢弃 |
| Close | 回复 Close 帧，设 close_received=true，recv 返回 -1 |

### 异常断开

Transport read 错误时：
- recv 返回 -1
- close_code 设为 1006（Abnormal Closure）
- 不尝试发送 close 帧

## 复用模块

| 模块 | 来源 | 用途 |
|------|------|------|
| ws-frame.h/c | 现有 src/net/ws/ | 帧头编解码、mask 运算、close payload 编解码 |
| ws-handshake.h/c | 现有 src/net/ws/ | key 生成、accept 计算、请求构造、响应验证 |
| ws-utf8.h/c | 现有 src/net/ws/ | TEXT 消息 UTF-8 校验 |
| http_transport_t | src/net/http/http.h | read/write/close/deadline vtable |

## 使用示例

### Echo 服务器（standalone）

```c
void echo_handler(xylem_ws_conn_t* ws, void* ud) {
    xylem_ws_msg_t msg;
    while (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_send(ws, msg.opcode, msg.data, msg.len);
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, NULL, 0);
}

void app_main(void* arg) {
    xylem_ws_listener_t* l = xylem_ws_listen("0.0.0.0", 8080,
                                              echo_handler, NULL, NULL);
    // ...
    xylem_ws_close_listener(l);
}
```

### 客户端

```c
void client_main(void* arg) {
    xylem_ws_conn_t* ws = xylem_ws_dial("ws://localhost:8080/chat", NULL);
    if (!ws) return;

    xylem_ws_send(ws, XYLEM_WS_TEXT, "hello", 5);

    xylem_ws_msg_t msg;
    if (xylem_ws_recv(ws, &msg) == 0) {
        printf("recv: %.*s\n", (int)msg.len, (char*)msg.data);
        xylem_ws_msg_free(&msg);
    }

    xylem_ws_close(ws, 1000, NULL, 0);
}
```

### WSS 客户端

```c
void secure_client(void* arg) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create(NULL);
    xylem_ws_conn_t* ws = xylem_wss_dial("wss://example.com/ws", ctx, NULL);
    if (!ws) { xylem_tls_ctx_destroy(ctx); return; }

    // ... send/recv ...

    xylem_ws_close(ws, 1000, NULL, 0);
    xylem_tls_ctx_destroy(ctx);
}
```
