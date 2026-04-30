# Platform

`#include <xylem/xylem-platform.h>`

平台特定类型定义，为公共 API 头文件提供所需的最小类型集合。

---

## 概述

此头文件提供 `sockaddr_storage`、`ssize_t`、`socklen_t` 等平台相关类型定义，供其他公共头文件（如 `xylem-addr.h`）使用。

- **Windows**：包含 `WinSock2.h` 和 `ws2ipdef.h`（定义 `WIN32_LEAN_AND_MEAN`）
- **Unix**：包含 `sys/socket.h`

!!! note
    此头文件仅提供类型定义，不包含任何函数。内部代码应使用 `src/platform/platform-socket.h` 获取完整的 socket API。
