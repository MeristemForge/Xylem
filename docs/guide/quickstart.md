# 5 分钟入门

本指南通过几个简单示例展示 xylem 的核心用法。

## 基本概念

xylem 的所有网络操作都围绕三个核心概念：

1. **事件循环**（`xylem_loop_t`）——驱动所有异步 I/O
2. **Handler 回调**——通过结构体注册事件处理函数
3. **句柄**（conn / server / udp）——代表一个网络资源

```
xylem_startup()
    └── xylem_loop_create()
            ├── xylem_tcp_listen() / xylem_tcp_dial()
            ├── xylem_udp_listen() / xylem_udp_dial()
            ├── xylem_tls_listen() / xylem_tls_dial()
            └── ...
            │
            xylem_loop_run()   ← 阻塞，直到所有句柄关闭或 stop
            │
    └── xylem_loop_destroy()
xylem_cleanup()
```

## 示例 1：TCP Echo 客户端

```c
#include <xylem.h>
#include <stdio.h>
#include <string.h>

static void on_connect(xylem_tcp_conn_t* conn) {
    printf("connected!\n");
    xylem_tcp_send(conn, "hello", 5);
}

static void on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    printf("received: %.*s\n", (int)len, (char*)data);
    xylem_tcp_close(conn);
}

static void on_close(xylem_tcp_conn_t* conn, int err, const char* msg) {
    printf("closed\n");
    xylem_loop_stop(xylem_tcp_get_loop(conn));
}

int main(void) {
    xylem_startup();
    xylem_loop_t* loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_resolve(&addr, "127.0.0.1", 8080);

    xylem_tcp_handler_t handler = {
        .on_connect = on_connect,
        .on_read    = on_read,
        .on_close   = on_close,
    };

    xylem_tcp_dial(loop, &addr, &handler, NULL);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
```

## 示例 2：UDP 收发

```c
#include <xylem.h>
#include <stdio.h>

static void on_read(xylem_udp_t* udp, void* data, size_t len,
                    xylem_addr_t* addr) {
    printf("received %zu bytes\n", len);
    // echo back to sender
    xylem_udp_send(udp, addr, data, len);
}

int main(void) {
    xylem_startup();
    xylem_loop_t* loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_resolve(&addr, "0.0.0.0", 9000);

    xylem_udp_handler_t handler = { .on_read = on_read };
    xylem_udp_listen(loop, &addr, &handler);

    xylem_loop_run(loop);
    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
```

## 示例 3：TCP 帧解析

xylem 内置帧解析器，自动从字节流中提取完整消息：

```c
// 固定 4 字节帧
xylem_tcp_opts_t opts = {
    .framing = {
        .type = XYLEM_TCP_FRAME_FIXED,
        .fixed.frame_size = 4,
    },
};

// on_read 回调保证每次收到恰好 4 字节
static void on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    // len == 4, always
}
```

支持的帧策略：

| 策略 | 说明 |
|------|------|
| `FRAME_NONE` | 无帧，原始字节流 |
| `FRAME_FIXED` | 固定长度帧 |
| `FRAME_LENGTH` | 长度前缀帧（支持大端/小端/varint） |
| `FRAME_DELIM` | 分隔符帧（如 `\r\n`） |
| `FRAME_CUSTOM` | 自定义解析函数 |

## 示例 4：跨线程发送

```c
static void on_connect(xylem_tcp_conn_t* conn) {
    // 在事件循环线程上 acquire
    xylem_tcp_conn_acquire(conn);

    // 传递给工作线程
    start_worker_thread(conn);
}

// 工作线程中：
void worker(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "hello from worker", 17);  // 线程安全
    xylem_tcp_conn_release(conn);  // 用完 release
}
```

## 下一步

- [API 参考](../api/index.md) — 完整的函数文档
- [设计文档](../design/tcp-design.md) — 深入了解内部实现
- [示例代码](https://github.com/user/xylem/tree/main/examples) — 更多完整示例
