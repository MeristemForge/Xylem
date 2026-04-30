# API 参考

Xylem SDK 的公共 API 按功能分为以下模块。点击模块名进入详细文档。

## 初始化

在使用任何 xylem API 之前，必须调用 `xylem_startup()`；程序退出前调用 `xylem_cleanup()`。

```c
#include <xylem.h>

int main(void) {
    xylem_startup();
    // ... 使用 xylem ...
    xylem_cleanup();
    return 0;
}
```

| 函数 | 说明 |
|------|------|
| `int xylem_startup(void)` | 初始化库。Windows 上调用 WSAStartup。成功返回 0。 |
| `void xylem_cleanup(void)` | 清理库。必须在所有资源释放后调用。 |

## 模块总览

### 核心

| 模块 | 头文件 | 说明 |
|------|--------|------|
| [事件循环](loop.md) | `xylem-loop.h` | I/O 多路复用、定时器、跨线程投递 |
| [地址](addr.md) | `xylem-addr.h` | 地址解析与转换 |
| [平台](platform.md) | `xylem-platform.h` | 平台检测宏 |
| [线程池](thrdpool.md) | `xylem-thrdpool.h` | 固定大小线程池 |

### 传输层

| 模块 | 头文件 | 说明 |
|------|--------|------|
| [TCP](tcp.md) | `xylem-tcp.h` | 异步 TCP，内置帧解析、重连、心跳 |
| [UDP](udp.md) | `xylem-udp.h` | 异步 UDP，listen / dial 双模式 |
| [UDS](uds.md) | `xylem-uds.h` | Unix Domain Socket |
| [TLS](tls.md) | `xylem-tls.h` | TLS 加密传输（基于 OpenSSL） |
| [DTLS](dtls.md) | `xylem-dtls.h` | DTLS 加密数据报（基于 OpenSSL） |
| [RUDP](rudp.md) | `xylem-rudp.h` | 可靠 UDP（KCP + FEC + AES-256） |

### 应用层

| 模块 | 头文件 | 说明 |
|------|--------|------|
| [HTTP 通用](http-common.md) | `xylem-http-common.h` | HTTP 请求/响应类型定义 |
| [HTTP 客户端](http-client.md) | `xylem-http-client.h` | 异步 HTTP 客户端 |
| [HTTP 服务端](http-server.md) | `xylem-http-server.h` | 异步 HTTP 服务端 |
| [WebSocket 通用](ws-common.md) | `xylem-ws-common.h` | WebSocket 帧类型定义 |
| [WebSocket 客户端](ws-client.md) | `xylem-ws-client.h` | 异步 WebSocket 客户端 |
| [WebSocket 服务端](ws-server.md) | `xylem-ws-server.h` | 异步 WebSocket 服务端 |

### 设备

| 模块 | 头文件 | 说明 |
|------|--------|------|
| [串口](serial.md) | `xylem-serial.h` | 跨平台同步串口通信 |

### 工具

| 模块 | 头文件 | 说明 |
|------|--------|------|
| [JSON](json.md) | `xylem-json.h` | JSON 解析与生成（基于 yyjson） |
| [Gzip](gzip.md) | `xylem-gzip.h` | Gzip 压缩/解压（基于 miniz） |
| [Base64](base64.md) | `xylem-base64.h` | Base64 编解码 |
| [AES-256](aes256.md) | `xylem-aes256.h` | AES-256-CTR 加密 |
| [SHA / HMAC](sha.md) | `xylem-sha1.h` `xylem-sha256.h` `xylem-hmac256.h` | 哈希与消息认证 |
| [FEC](fec.md) | `xylem-fec.h` | Reed-Solomon 前向纠错 |
| [Varint](varint.md) | `xylem-varint.h` | 变长整数编解码 |
| [日志](logger.md) | `xylem-logger.h` | 分级日志输出 |

### 数据结构

| 模块 | 头文件 | 说明 |
|------|--------|------|
| [链表](list.md) | `xylem-list.h` | 侵入式双向链表 |
| [队列](queue.md) | `xylem-queue.h` | 侵入式 FIFO 队列 |
| [栈](stack.md) | `xylem-stack.h` | 侵入式栈 |
| [堆](heap.md) | `xylem-heap.h` | 侵入式二叉最小堆 |
| [红黑树](rbtree.md) | `xylem-rbtree.h` | 侵入式红黑树 |
| [环形缓冲区](ringbuf.md) | `xylem-ringbuf.h` | 固定大小环形缓冲区 |

## 约定

- 所有异步操作通过 handler 回调通知结果
- `send` 和 `close` 函数标注为 **线程安全**，可从任意线程调用
- 其他 API 必须在事件循环线程上调用
- 返回 `0` 表示成功，`-1` 表示失败（除非文档另有说明）
- 指针参数标注 `NULL` 含义时，`NULL` 表示使用默认值
