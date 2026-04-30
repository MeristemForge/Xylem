# Serial 模块测试设计文档

## 概述

`tests/test-serial.c` 包含 10 个测试函数，覆盖 `src/xylem-serial.c` 的参数校验和 NULL 安全性。

由于串口测试需要真实硬件设备或虚拟串口对（如 `socat`），CI 环境中无法可靠提供，因此测试仅覆盖不需要打开真实设备的路径。实际 I/O 功能由 `examples/serial-terminal.c` 示例程序在手动测试中验证。

## 测试基础设施

- 无共享状态，每个测试函数独立
- 无事件循环、无定时器、无异步回调
- 所有测试均为同步的纯函数调用 + ASSERT
- `main` 函数首尾调用 `xylem_startup` 和 `xylem_cleanup`

## 测试列表

### open 参数校验（7 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_open_null_opts` | opts==NULL | 返回 NULL |
| `test_open_null_device` | opts->device==NULL | 返回 NULL |
| `test_open_invalid_baudrate` | baudrate 越界（99） | 返回 NULL |
| `test_open_invalid_parity` | parity 越界（99） | 返回 NULL |
| `test_open_invalid_databits` | databits 越界（99） | 返回 NULL |
| `test_open_invalid_stopbits` | stopbits 越界（99） | 返回 NULL |
| `test_open_invalid_flowcontrol` | flowcontrol 越界（99） | 返回 NULL |

### close NULL 安全（1 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_close_null` | close(NULL) | 不崩溃 |

### read/write NULL 句柄（2 个）

| 测试函数 | 覆盖的功能 | 验证点 |
|---------|-----------|--------|
| `test_read_null` | read(NULL, ...) | 返回 -1 |
| `test_write_null` | write(NULL, ...) | 返回 -1 |

## 覆盖的公共 API

| API 函数 | 覆盖的测试 |
|---------|-----------|
| `xylem_serial_open` | test_open_null_opts, test_open_null_device, test_open_invalid_baudrate, test_open_invalid_parity, test_open_invalid_databits, test_open_invalid_stopbits, test_open_invalid_flowcontrol |
| `xylem_serial_close` | test_close_null |
| `xylem_serial_read` | test_read_null |
| `xylem_serial_write` | test_write_null |

## 覆盖的内部分支

| 内部路径 | 覆盖的分支 | 触发测试 |
|---------|-----------|---------|
| `xylem_serial_open` | opts==NULL 提前返回 | `test_open_null_opts` |
| `xylem_serial_open` | device==NULL 提前返回 | `test_open_null_device` |
| `xylem_serial_open` | baudrate 越界提前返回 | `test_open_invalid_baudrate` |
| `xylem_serial_open` | parity 越界提前返回 | `test_open_invalid_parity` |
| `xylem_serial_open` | databits 越界提前返回 | `test_open_invalid_databits` |
| `xylem_serial_open` | stopbits 越界提前返回 | `test_open_invalid_stopbits` |
| `xylem_serial_open` | flowcontrol 越界提前返回 | `test_open_invalid_flowcontrol` |
| `xylem_serial_close` | serial==NULL 提前返回 | `test_close_null` |
| `xylem_serial_read` | serial==NULL 返回 -1 | `test_read_null` |
| `xylem_serial_write` | serial==NULL 返回 -1 | `test_write_null` |

## 未覆盖的路径

| 路径 | 原因 |
|------|------|
| `xylem_serial_open` 成功路径（平台层打开设备） | 需要真实串口设备或虚拟串口对 |
| `xylem_serial_close` 正常关闭路径 | 需要先成功 open |
| `xylem_serial_read` 正常读取路径 | 需要已打开的串口 |
| `xylem_serial_write` 正常写入路径 | 需要已打开的串口 |
| `platform_serial_open` 失败路径（设备不存在） | 平台相关，`/dev/null` 在 Unix 上可能成功打开但配置失败 |
| `calloc` 失败路径 | 需要 mock 内存分配失败 |
| 超时精度对齐（100ms 向上取整） | 需要真实串口设备验证实际超时行为 |
| 硬件流控（RTS/CTS）实际行为 | 需要支持流控的硬件 |
