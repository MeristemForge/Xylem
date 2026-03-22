# Implementation Plan: HTTP 模块

## Overview

为 Xylem 库实现 HTTP/1.1 模块，包含同步客户端 API 和异步服务器 API。按增量顺序实现：先构建基础组件（URL 解析、percent-encoding、请求序列化、响应解析），再实现客户端 API，最后实现服务器 API。每个阶段都包含对应的测试任务。

## Tasks

- [x] 1. 项目结构搭建与 llhttp 构建集成
  - [x] 1.1 创建 `include/xylem/xylem-http.h` 头文件，包含所有公共类型前向声明（`xylem_http_res_t`、`xylem_http_req_t`、`xylem_http_conn_t`、`xylem_http_srv_t`）、`xylem_http_srv_cfg_t` 结构体定义、回调函数类型定义、以及所有公共函数的 `extern` 声明（含 Doxygen 注释）
    - 遵循 `_Pragma("once")`、license header、opaque struct 前向声明模式
    - _Requirements: 7.1-7.6, 8.2, 8.3, 12.1, 12.7_
  - [x] 1.2 创建 `src/xylem-http.c` 骨架文件，包含 license header、必要的 includes（`llhttp.h`、`xylem-tcp.h`、`xylem-tls.h`、`xylem-addr.h`、`xylem-loop.h`、`xylem-thrdpool.h`）、所有内部结构体定义（`_http_url_t`、`_http_header_t`、`xylem_http_res_s`、`xylem_http_req_s`、`xylem_http_conn_s`、`xylem_http_srv_s`、`_http_client_ctx_t`）、以及线程局部全局配置变量
    - _Requirements: 12.2, 14.5, 18.3, 18.5_
  - [x] 1.3 修改根 `CMakeLists.txt`：将 `src/xylem-http.c` 和 llhttp 源文件（`src/http/llhttp/llhttp.c`、`src/http/llhttp/http.c`、`src/http/llhttp/api.c`）添加到 SRCS 列表，将 `src/http/llhttp/` 添加到 include_directories
    - _Requirements: 1.1, 1.2, 1.3, 12.3, 12.4_
  - [x] 1.4 修改 `include/xylem.h`：添加 `#include "xylem/xylem-http.h"`
    - _Requirements: 12.6_
  - [x] 1.5 创建 `tests/test-http.c` 骨架文件（license header、`#include "assert.h"`、`#include "xylem.h"`、空 `main`），并在 `tests/CMakeLists.txt` 中添加 `xylem_add_test(http)`
    - _Requirements: 12.5_

- [x] 2. Checkpoint - 确保项目编译通过
  - 确保 llhttp 源文件和 HTTP 模块骨架编译通过，ask the user if questions arise.

- [x] 3. URL 解析器与 percent-encoding
  - [x] 3.1 在 `src/xylem-http.c` 中实现 `_http_url_parse` 静态函数：从 URL 字符串中提取 scheme（仅 http/https）、host、port（http 默认 80、https 默认 443、支持显式端口）、path（默认 "/"），无效 URL 返回 -1
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_
  - [x] 3.2 在 `src/xylem-http.c` 中实现 `_http_url_serialize` 静态函数：将 `_http_url_t` 重新拼接为 URL 字符串
    - _Requirements: 2.7_
  - [x] 3.3 在 `src/xylem-http.c` 中实现 `xylem_http_url_encode` 和 `xylem_http_url_decode` 公共函数
    - _Requirements: 21.1, 21.2, 21.3, 21.4_
  - [x] 3.4 在 `tests/test-http.c` 中添加 URL 解析单元测试：`test_url_parse_http_default_port`、`test_url_parse_https_default_port`、`test_url_parse_explicit_port`、`test_url_parse_no_path`、`test_url_parse_invalid`
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_
  - [ ]* 3.5 在 `tests/test-http.c` 中添加属性测试：URL parse-serialize round-trip
    - **Property 1: URL parse-serialize round-trip**
    - 使用 theft 库生成随机有效 URL 组件（scheme、host、port、path），验证 parse -> serialize -> parse 产生等价结果
    - **Validates: Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.7, 2.8**
  - [ ]* 3.6 在 `tests/test-http.c` 中添加属性测试：Invalid URL rejection
    - **Property 2: Invalid URL rejection**
    - 使用 theft 库生成随机无效 URL 字符串（缺少 scheme、不支持的 scheme、空 host），验证 `_http_url_parse` 返回 -1
    - **Validates: Requirements 2.6**
  - [ ]* 3.7 在 `tests/test-http.c` 中添加属性测试：URL percent-encoding round-trip
    - **Property 9: URL percent-encoding round-trip**
    - 使用 theft 库生成随机字节序列，验证 encode -> decode 产生原始字节序列
    - **Validates: Requirements 21.1, 21.2, 21.3, 21.5**

- [x] 4. HTTP 请求序列化器
  - [x] 4.1 在 `src/xylem-http.c` 中实现 `_http_req_serialize` 静态函数：将 method、URL、headers、body 组装为 HTTP/1.1 请求报文，自动填充 Host、Content-Length、Connection: keep-alive，body > 1024 字节时添加 Expect: 100-continue，返回 `xylem_buf_t`
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 20.1_
  - [ ]* 4.2 在 `tests/test-http.c` 中添加属性测试：HTTP request serialize-parse round-trip
    - **Property 3: HTTP request serialize-parse round-trip**
    - 使用 theft 库生成随机有效请求组件（method、host、port、path、body），序列化后通过 llhttp 解析，验证 method、URL path、body 等价
    - **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6**
  - [ ]* 4.3 在 `tests/test-http.c` 中添加属性测试：100-Continue header for large bodies
    - **Property 8: 100-Continue header for large bodies**
    - 使用 theft 库生成随机大小的 body，验证 > 1024 字节时序列化结果包含 Expect: 100-continue，<= 1024 字节时不包含
    - **Validates: Requirements 20.1**

- [x] 5. HTTP 响应解析与 Header 管理
  - [x] 5.1 在 `src/xylem-http.c` 中实现 llhttp 响应解析回调集（`on_header_field`、`on_header_value`、`on_header_value_complete`、`on_headers_complete`、`on_body`、`on_message_complete`），支持 Content-Length 和 chunked transfer encoding，支持 max_body_size 检查
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 18.3, 18.4_
  - [x] 5.2 在 `src/xylem-http.c` 中实现 Header 大小写不敏感查找辅助函数 `_http_header_find`，使用逐字符 ASCII tolower 比较
    - _Requirements: 11.1, 11.2, 11.3_
  - [x] 5.3 在 `src/xylem-http.c` 中实现响应访问器公共函数：`xylem_http_res_status`、`xylem_http_res_header`、`xylem_http_res_body`、`xylem_http_res_body_len`、`xylem_http_res_destroy`（NULL 安全）
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7_
  - [x] 5.4 在 `tests/test-http.c` 中添加单元测试：`test_res_destroy_null`、`test_header_lookup_missing`
    - _Requirements: 7.3, 7.7_
  - [ ]* 5.5 在 `tests/test-http.c` 中添加属性测试：Duplicate header preservation
    - **Property 4: Duplicate header preservation**
    - 构造包含重复 header 的 HTTP 响应字节流，通过解析器解析后验证所有 header 值均被保留
    - **Validates: Requirements 4.4**
  - [ ]* 5.6 在 `tests/test-http.c` 中添加属性测试：Malformed response rejection
    - **Property 5: Malformed response rejection**
    - 使用 theft 库生成随机非法 HTTP 响应字节流，验证解析器返回错误
    - **Validates: Requirements 4.5**
  - [ ]* 5.7 在 `tests/test-http.c` 中添加属性测试：Case-insensitive header lookup
    - **Property 6: Case-insensitive header lookup**
    - 使用 theft 库生成随机 header name 及其大小写变体，验证查找返回相同值
    - **Validates: Requirements 7.2, 9.4, 11.1, 11.2, 11.3**

- [x] 6. Checkpoint - 确保基础组件测试通过
  - 确保 URL 解析、请求序列化、响应解析的所有测试通过，ask the user if questions arise.

- [x] 7. Reason phrase 映射与全局配置函数
  - [x] 7.1 在 `src/xylem-http.c` 中实现 `_http_reason_phrase` 静态函数：switch-case 将状态码映射到标准 reason phrase（1xx-5xx），未知状态码返回空字符串
    - _Requirements: 15.1, 15.2, 15.3_
  - [x] 7.2 在 `src/xylem-http.c` 中实现全局配置公共函数：`xylem_http_set_timeout`、`xylem_http_set_follow_redirects`、`xylem_http_set_max_body_size`
    - _Requirements: 14.1, 14.3, 16.2, 18.6_
  - [x] 7.3 在 `tests/test-http.c` 中添加单元测试：`test_reason_phrase_known`、`test_reason_phrase_unknown`、`test_default_timeout`、`test_default_max_body_client`
    - _Requirements: 14.5, 15.1, 15.3, 18.3_

- [x] 8. 同步客户端核心执行引擎
  - [x] 8.1 在 `src/xylem-http.c` 中实现 `_http_client_exec` 静态函数：创建临时 xylem_loop 和 thrdpool，执行 DNS 解析、TCP/TLS 连接（根据 scheme 自动选择）、请求发送、响应接收，支持超时定时器覆盖全流程，使用 goto cleanup 模式确保资源释放
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 14.2, 14.4_
  - [x] 8.2 在 `_http_client_exec` 中实现 100-Continue 处理逻辑：body > 1024 字节时先发送 headers，等待 100 响应或 1 秒超时后发送 body；收到非 100 响应时直接返回该响应
    - _Requirements: 20.1, 20.2, 20.3, 20.5_
  - [x] 8.3 在 `_http_client_exec` 中实现重定向处理逻辑：当 `_http_max_redirects > 0` 且响应为 301/302/307/308 时，提取 Location header 发起新请求；301/302 改用 GET 丢弃 body，307/308 保留原方法和 body；超过最大次数返回最后响应
    - _Requirements: 16.1, 16.3, 16.4, 16.5, 16.6_
  - [x] 8.4 在 `_http_client_exec` 中实现 Connection: close 处理：收到 `Connection: close` 响应头后关闭连接
    - _Requirements: 13.1_

- [x] 9. 客户端公共请求函数
  - [x] 9.1 在 `src/xylem-http.c` 中实现 `xylem_http_get`：调用 `_http_client_exec` 发送 GET 请求
    - _Requirements: 5.1_
  - [x] 9.2 在 `src/xylem-http.c` 中实现 `xylem_http_post` 和 `xylem_http_post_json`：调用 `_http_client_exec` 发送 POST 请求，`post_json` 自动设置 Content-Type 为 "application/json"
    - _Requirements: 6.1, 6.2, 6.3, 6.4_
  - [x] 9.3 在 `src/xylem-http.c` 中实现 `xylem_http_put`、`xylem_http_delete`、`xylem_http_patch`
    - _Requirements: 17.1, 17.2, 17.3, 17.4_
  - [ ] 9.4 在 `tests/test-http.c` 中添加单元测试：`test_empty_body_post`、`test_redirect_301_changes_method`、`test_redirect_307_preserves_method`
    - _Requirements: 6.4, 16.5, 16.6_

- [x] 10. Checkpoint - 确保客户端编译通过
  - 确保客户端所有代码编译通过，ask the user if questions arise.

- [x] 11. HTTP 服务器实现
  - [x] 11.1 在 `src/xylem-http.c` 中实现 `xylem_http_srv_create`（NULL loop/cfg 返回 NULL）和 `xylem_http_srv_destroy`
    - _Requirements: 8.1, 8.2, 8.8, 8.9_
  - [x] 11.2 在 `src/xylem-http.c` 中实现 `xylem_http_srv_start`：根据 cfg 中 tls_cert/tls_key 选择 xylem_tcp_listen 或 xylem_tls_listen 绑定端口，设置 on_accept 回调，绑定失败返回 -1
    - _Requirements: 8.4, 8.5, 8.6, 8.10_
  - [x] 11.3 在 `src/xylem-http.c` 中实现 `xylem_http_srv_stop`
    - _Requirements: 8.7_
  - [x] 11.4 在 `src/xylem-http.c` 中实现服务器连接管理：on_accept 中分配 `xylem_http_conn_t`，初始化 llhttp（HTTP_REQUEST 类型）和空闲定时器，实现 llhttp 请求解析回调，解析完成后调用用户 on_request
    - _Requirements: 9.1, 13.3, 13.4, 19.1, 19.4_
  - [x] 11.5 在 `src/xylem-http.c` 中实现请求访问器公共函数：`xylem_http_req_method`、`xylem_http_req_url`、`xylem_http_req_header`、`xylem_http_req_body`、`xylem_http_req_body_len`
    - _Requirements: 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8_
  - [x] 11.6 在 `src/xylem-http.c` 中实现 `xylem_http_conn_send`：序列化 HTTP/1.1 响应（状态行含 reason phrase、Content-Type、Content-Length、body），连接已关闭返回 -1；以及 `xylem_http_conn_close`
    - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5, 15.1_
  - [x] 11.7 在 `src/xylem-http.c` 中实现服务器端 keep-alive 和 Connection: close 处理：响应发送后检查 keep-alive 标志，是则重置解析器和空闲定时器继续读取，否则关闭连接
    - _Requirements: 13.2, 13.3, 13.4, 13.5_
  - [x] 11.8 在 `src/xylem-http.c` 中实现服务器端 body 大小限制检查（默认 1 MiB）和 100-Continue 自动响应
    - _Requirements: 18.1, 18.2, 18.5, 19.2, 19.3, 20.4_
  - [x] 11.9 在 `tests/test-http.c` 中添加单元测试：`test_srv_create_null_loop`、`test_srv_create_null_cfg`、`test_default_max_body_server`、`test_idle_timeout_default`
    - _Requirements: 8.9, 18.1, 19.2_
  - [ ]* 11.10 在 `tests/test-http.c` 中添加属性测试：Server response serialization validity
    - **Property 7: Server response serialization validity**
    - 使用 theft 库生成随机状态码、content type、body，验证 `xylem_http_conn_send` 产生的响应包含正确的状态行（含 reason phrase）、Content-Type、Content-Length 和 body
    - **Validates: Requirements 10.1, 15.1, 15.2, 15.3**

- [ ] 12. Checkpoint - 确保所有测试通过
  - 确保所有单元测试和属性测试通过，ask the user if questions arise.

- [ ] 13. 集成测试
  - [ ]* 13.1 在 `tests/test-http.c` 中添加集成测试：在同一进程中启动 HTTP 服务器和客户端，验证 GET/POST 请求-响应周期、keep-alive 多请求复用、Connection: close 语义
    - _Requirements: 5.1, 6.1, 9.1, 10.1, 13.1, 13.2, 13.3, 13.4_

- [ ] 14. Final checkpoint - 确保所有测试通过
  - 确保所有测试通过，ask the user if questions arise.

## Notes

- 标记 `*` 的任务为可选任务，可跳过以加速 MVP 开发
- 属性测试使用 theft 库（通过 CMake FetchContent 集成），每个属性至少运行 100 次迭代
- 每个任务引用具体的 requirements 编号以确保可追溯性
- Checkpoints 确保增量验证，避免问题累积
- 所有 .c 和 .h 文件必须包含 license header
- 遵循 docs/style.md 中的命名规范、文件组织规范和测试规范

- [ ] 15. 自定义 HTTP 头部支持
  - [x] 15.1 创建 `include/xylem/http/xylem-http-common.h` 公共头文件，定义 `xylem_http_hdr_t` 类型（含 `const char* name` 和 `const char* value` 字段），包含 license header、`_Pragma("once")`、Doxygen 注释。在 `include/xylem.h` 中添加 `#include "xylem/http/xylem-http-common.h"`
    - **实际变更**: 将 `xylem-http-url.h/c` 重命名为 `xylem-http-utils.h/c`，`xylem_http_hdr_t` 定义在 `xylem-http-utils.h` 中，未创建 `xylem-http-common.h`
    - _Requirements: 22.1, 23.1_

  - [x] 15.2 更新 `include/xylem/http/xylem-http-client.h`：添加 `#include "xylem/http/xylem-http-utils.h"`，在 `xylem_http_cli_opts_t` 结构体中新增 `const xylem_http_hdr_t* headers` 和 `size_t header_count` 字段，更新 Doxygen 注释说明零初始化时行为不变
    - _Requirements: 22.1_

  - [x] 15.3 更新 `src/http/http-common.h` 和 `src/http/http-common.c`：修改 `http_req_serialize` 函数签名，新增 `const xylem_http_hdr_t* custom_headers` 和 `size_t custom_header_count` 参数。更新实现：先写自定义头部，再写自动生成头部（Host、Content-Length、Content-Type、Connection、Expect），使用 `http_header_eq` 进行大小写不敏感覆盖检测，跳过被自定义头部覆盖的自动生成头部。更新 buffer 大小估算以包含自定义头部长度
    - _Requirements: 22.2, 22.3, 22.4, 22.5_

  - [x] 15.4 更新 `src/http/xylem-http-client.c`：在 `_http_client_ctx_t` 中新增 `custom_headers` 和 `custom_header_count` 字段。在 `_http_client_exec` 中从 opts 提取自定义头部。在 `_http_client_connect_cb` 中将自定义头部传递给 `http_req_serialize`
    - _Requirements: 22.6_

  - [x] 15.5 更新 `include/xylem/http/xylem-http-server.h`：添加 `#include "xylem/http/xylem-http-utils.h"`，修改 `xylem_http_conn_send` 函数签名，新增 `const xylem_http_hdr_t* headers` 和 `size_t header_count` 参数，更新 Doxygen 注释
    - _Requirements: 23.1_

  - [x] 15.6 更新 `src/http/xylem-http-server.c`：更新 `xylem_http_conn_send` 实现，先写自定义响应头部，再写自动生成头部（Content-Type、Content-Length），使用 `http_header_eq` 检测覆盖，跳过被自定义头部覆盖的自动生成头部。传 NULL/0 时行为与现有实现一致
    - **注意**: 当前为 stub 实现，完整实现在 Task 11
    - _Requirements: 23.2, 23.3, 23.4, 23.5_

  - [x] 15.7 在 `tests/test-http.c` 中添加自定义头部单元测试：通过 `http_req_serialize`（包含 `src/http/http-common.h` 内部头文件）直接测试序列化行为。添加测试函数 `test_req_serialize_custom_headers`（验证自定义头部出现在输出中）、`test_req_serialize_override_host`（验证自定义 Host 覆盖自动生成的 Host）、`test_req_serialize_no_custom_headers`（验证 header_count=0 时输出与原实现一致）。在 `tests/CMakeLists.txt` 中为 test-http 添加 `src/http/` 到 include path（如需要）
    - _Requirements: 22.2, 22.3, 22.4, 22.5, 24.1, 24.3_

  - [ ]* 15.8 在 `tests/test-http.c` 中添加属性测试：自定义请求头序列化正确性
    - **Property 10: 自定义请求头序列化正确性**
    - 使用 theft 库生成随机有效的 header name-value 对和请求组件，验证：(a) 所有自定义头部出现在序列化输出中，(b) 自定义头部在自动生成头部之前，(c) 覆盖检测正确工作，(d) 无覆盖时自动生成头部正常出现
    - **Validates: Requirements 22.2, 22.3, 22.4, 22.5**

  - [ ]* 15.9 在 `tests/test-http.c` 中添加属性测试：自定义头部 round-trip
    - **Property 11: 自定义头部 round-trip**
    - 使用 theft 库生成随机有效的 header name-value 对，序列化 HTTP 请求后通过 llhttp 解析，验证所有自定义头部名称和值被正确恢复；当自定义头部覆盖自动生成头部时，解析结果包含自定义值而非自动生成值
    - **Validates: Requirements 24.1, 24.2, 24.3**

- [x] 16. Checkpoint - 确保自定义头部编译通过并测试通过
  - 所有自定义头部相关代码编译通过，所有新增单元测试通过（30/30）。

- [ ] 17. 服务器端 Chunked Transfer Encoding 响应
  - [ ] 17.1 在 `include/xylem/http/xylem-http-server.h` 中添加 `xylem_http_conn_start_chunked`、`xylem_http_conn_send_chunk`、`xylem_http_conn_end_chunked` 函数声明（含 Doxygen 注释）
    - _Requirements: 25.1, 25.2, 25.3_
  - [ ] 17.2 在 `src/http/xylem-http-server.c` 的 `xylem_http_conn_s` 中新增 `bool chunked_active` 字段
    - _Requirements: 25.1_
  - [ ] 17.3 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_start_chunked`：序列化 status line + Transfer-Encoding: chunked + 自定义 headers + Content-Type（不含 Content-Length），发送后设置 `chunked_active = true`
    - _Requirements: 25.1, 25.4, 25.7_
  - [ ] 17.4 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_send_chunk`：格式化 `{hex_size}\r\n{data}\r\n` 并发送，len=0 时 no-op 返回 0，连接关闭或非 chunked 模式返回 -1
    - _Requirements: 25.2, 25.5, 25.7_
  - [ ] 17.5 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_end_chunked`：发送 `0\r\n\r\n`，设置 `chunked_active = false`，处理 keep-alive（重置解析器或关闭连接）
    - _Requirements: 25.3, 25.6, 25.7_
  - [ ] 17.6 在 `tests/test-http.c` 中添加单元测试：`test_chunked_send_empty`（len=0 no-op）、`test_chunked_on_closed`（关闭连接后返回 -1）
    - _Requirements: 25.5, 25.7_
  - [ ]* 17.7 在 `tests/test-http.c` 中添加属性测试：Chunked response 格式正确性
    - **Property 12: Chunked response format correctness**
    - **Validates: Requirements 25.1, 25.2, 25.3, 25.4**

- [ ] 18. Checkpoint - 确保 Chunked 功能编译通过并测试通过

- [ ] 19. 客户端 Cookie 管理
  - [ ] 19.1 在 `include/xylem/http/xylem-http-client.h` 中添加 `xylem_http_cookie_jar_t` 前向声明、`xylem_http_cookie_jar_create`、`xylem_http_cookie_jar_destroy` 函数声明，在 `xylem_http_cli_opts_t` 中新增 `xylem_http_cookie_jar_t* cookie_jar` 字段
    - _Requirements: 26.1, 26.2, 26.3, 26.9_
  - [ ] 19.2 在 `src/http/xylem-http-client.c` 中定义 `_http_cookie_t` 和 `xylem_http_cookie_jar_s` 内部结构体，实现 `xylem_http_cookie_jar_create` 和 `xylem_http_cookie_jar_destroy`
    - _Requirements: 26.1, 26.2_
  - [ ] 19.3 在 `src/http/xylem-http-client.c` 中实现 `_http_cookie_parse` 静态函数：从 Set-Cookie header value 中解析 name、value、Domain、Path、Expires、Max-Age、Secure、HttpOnly 属性
    - _Requirements: 26.4_
  - [ ] 19.4 在 `src/http/xylem-http-client.c` 中实现 `_http_cookie_match` 静态函数：根据 domain（尾部匹配）、path（前缀匹配）、secure（仅 HTTPS）、expires（惰性过期检查）判断 cookie 是否匹配当前请求
    - _Requirements: 26.5, 26.6, 26.7, 26.8_

  - [ ] 19.5 在 `src/http/xylem-http-client.c` 的 `_http_client_exec` 中集成 cookie 管理：请求发送前从 jar 匹配 cookie 拼接 Cookie header，响应接收后解析 Set-Cookie 存入 jar
    - _Requirements: 26.5, 26.4_
  - [ ] 19.6 在 `tests/test-http.c` 中添加单元测试：`test_cookie_jar_create_destroy`、`test_cookie_parse_basic`、`test_cookie_match_domain`、`test_cookie_match_path`、`test_cookie_secure_flag`、`test_cookie_expired`
    - _Requirements: 26.1, 26.2, 26.4, 26.5, 26.6, 26.7, 26.8_
  - [ ]* 19.7 在 `tests/test-http.c` 中添加属性测试：Cookie Set-Cookie parse round-trip
    - **Property 13: Cookie Set-Cookie parse round-trip**
    - **Validates: Requirements 26.4, 26.5, 26.6, 26.7**

- [ ] 20. Checkpoint - 确保 Cookie 功能编译通过并测试通过

- [ ] 21. Range 请求支持
  - [ ] 21.1 在 `include/xylem/http/xylem-http-client.h` 的 `xylem_http_cli_opts_t` 中新增 `const char* range` 字段
    - _Requirements: 27.1, 27.7_
  - [ ] 21.2 在 `src/http/xylem-http-client.c` 的 `_http_client_exec` 中：当 opts->range 非 NULL 时，将 `Range: {value}` 作为额外自定义 header 传入序列化器
    - _Requirements: 27.2_
  - [ ] 21.3 在 `include/xylem/http/xylem-http-server.h` 中添加 `xylem_http_conn_send_partial` 函数声明（含 Doxygen 注释）
    - _Requirements: 27.3, 27.4_
  - [ ] 21.4 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_send_partial`：验证 range 有效性，有效时发送 206 + Content-Range header，无效时发送 416 + Content-Range: bytes */{total}
    - _Requirements: 27.5, 27.6_
  - [ ] 21.5 在 `tests/test-http.c` 中添加单元测试：`test_range_206_response`、`test_range_416_invalid`
    - _Requirements: 27.5, 27.6_
  - [ ]* 21.6 在 `tests/test-http.c` 中添加属性测试：Range 响应 Content-Range 正确性
    - **Property 17: Range response Content-Range correctness**
    - **Validates: Requirements 27.5, 27.6**

- [ ] 22. Checkpoint - 确保 Range 功能编译通过并测试通过

- [ ] 23. 服务器端 CORS 支持
  - [ ] 23.1 在 `include/xylem/http/xylem-http-utils.h` 中添加 `xylem_http_cors_t` 结构体定义和 `xylem_http_cors_headers` 函数声明（含 Doxygen 注释）
    - _Requirements: 28.1, 28.2_
  - [ ] 23.2 在 `src/http/xylem-http-utils.c` 中实现 `xylem_http_cors_headers`：origin 匹配（`"*"` 通配符和逗号分隔精确列表）、输出 Access-Control-Allow-Origin、处理 credentials（不用 `"*"`）、preflight 额外 headers
    - _Requirements: 28.3, 28.4, 28.5, 28.6, 28.7_

  - [ ] 23.3 在 `tests/test-http.c` 中添加单元测试：`test_cors_wildcard_origin`、`test_cors_specific_origin`、`test_cors_credentials_no_wildcard`、`test_cors_preflight_headers`、`test_cors_null_config`
    - _Requirements: 28.3, 28.4, 28.5, 28.7_
  - [ ]* 23.4 在 `tests/test-http.c` 中添加属性测试：CORS header 生成正确性
    - **Property 16: CORS header generation correctness**
    - **Validates: Requirements 28.3, 28.4**

- [ ] 24. Checkpoint - 确保 CORS 功能编译通过并测试通过

- [ ] 25. 服务器端 SSE (Server-Sent Events)
  - [ ] 25.1 在 `include/xylem/http/xylem-http-server.h` 中添加 `xylem_http_conn_start_sse`、`xylem_http_conn_send_event`、`xylem_http_conn_send_sse_data`、`xylem_http_conn_end_sse` 函数声明（含 Doxygen 注释）
    - _Requirements: 29.1, 29.2, 29.3, 29.5_
  - [ ] 25.2 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_start_sse`：内部调用 `xylem_http_conn_start_chunked` 发送 status 200 + Content-Type: text/event-stream + Cache-Control: no-cache + Connection: keep-alive
    - _Requirements: 29.1, 29.6_
  - [ ] 25.3 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_send_event`：构建 SSE 格式字符串（event: + data: 行 + 空行），多行 data 按 `\n` 分割为多个 `data:` 行，通过 `xylem_http_conn_send_chunk` 发送
    - _Requirements: 29.2, 29.4, 29.7_
  - [ ] 25.4 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_conn_send_sse_data`（调用 send_event(conn, NULL, data)）和 `xylem_http_conn_end_sse`（调用 end_chunked）
    - _Requirements: 29.3, 29.5_
  - [ ] 25.5 在 `tests/test-http.c` 中添加单元测试：`test_sse_start_end`、`test_sse_event_format`、`test_sse_multiline_data`
    - _Requirements: 29.1, 29.2, 29.4_

- [ ] 26. Checkpoint - 确保 SSE 功能编译通过并测试通过

- [ ] 27. 服务器端 Multipart/Form-Data 解析
  - [ ] 27.1 在 `include/xylem/http/xylem-http-utils.h` 中添加 `xylem_http_multipart_t` 前向声明和所有 multipart 公共函数声明（parse、count、name、filename、content_type、data、data_len、destroy），含 Doxygen 注释
    - _Requirements: 30.1, 30.2, 30.3, 30.4, 30.5, 30.6, 30.7, 30.8_
  - [ ] 27.2 在 `src/http/xylem-http-utils.c` 中定义 `_http_multipart_part_t` 和 `xylem_http_multipart_s` 内部结构体，实现 boundary 提取（从 Content-Type 中查找 `boundary=`）
    - _Requirements: 30.9_
  - [ ] 27.3 在 `src/http/xylem-http-utils.c` 中实现 `xylem_http_multipart_parse`：按 boundary 分割 body，解析每个 part 的 Content-Disposition（name、filename）和 Content-Type，拷贝 part body 数据
    - _Requirements: 30.2, 30.4, 30.5, 30.6, 30.7, 30.9, 30.10_
  - [ ] 27.4 在 `src/http/xylem-http-utils.c` 中实现 multipart 访问器函数（count、name、filename、content_type、data、data_len）和 destroy
    - _Requirements: 30.3, 30.4, 30.5, 30.6, 30.7, 30.8_
  - [ ] 27.5 在 `tests/test-http.c` 中添加单元测试：`test_multipart_parse_basic`、`test_multipart_with_filename`、`test_multipart_invalid_boundary`、`test_multipart_destroy_null`
    - _Requirements: 30.2, 30.5, 30.9, 30.8_
  - [ ]* 27.6 在 `tests/test-http.c` 中添加属性测试：Multipart boundary extraction and part splitting
    - **Property 14: Multipart boundary extraction and part splitting**
    - **Validates: Requirements 30.2, 30.3, 30.4, 30.5, 30.6, 30.7**

- [ ] 28. Checkpoint - 确保 Multipart 功能编译通过并测试通过

- [ ] 29. 服务器端路由系统
  - [ ] 29.1 在 `include/xylem/http/xylem-http-server.h` 中添加 `xylem_http_router_t` 前向声明和 `xylem_http_router_create`、`xylem_http_router_destroy`、`xylem_http_router_add`、`xylem_http_router_dispatch` 函数声明（含 Doxygen 注释）
    - _Requirements: 31.1, 31.2, 31.3, 31.5, 31.8_
  - [ ] 29.2 在 `src/http/xylem-http-server.c` 中定义 `_http_route_t` 和 `xylem_http_router_s` 内部结构体，实现 `xylem_http_router_create` 和 `xylem_http_router_destroy`
    - _Requirements: 31.1, 31.2_
  - [ ] 29.3 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_router_add`：解析 pattern（检测尾部 `"*"` 标记前缀匹配），存入路由数组，重复路由返回 -1
    - _Requirements: 31.3, 31.4, 31.9, 31.10_
  - [ ] 29.4 在 `src/http/xylem-http-server.c` 中实现 `xylem_http_router_dispatch`：遍历路由表匹配 method + path，精确匹配优先于前缀匹配，最长前缀优先，method 精确优先于 NULL 通配，无匹配时发送 404
    - _Requirements: 31.5, 31.6, 31.7, 31.9_
  - [ ] 29.5 在 `tests/test-http.c` 中添加单元测试：`test_router_exact_match`、`test_router_prefix_match`、`test_router_exact_over_prefix`、`test_router_longest_prefix`、`test_router_404`、`test_router_method_null`、`test_router_destroy_null`
    - _Requirements: 31.2, 31.4, 31.5, 31.6, 31.7, 31.9_
  - [ ]* 29.6 在 `tests/test-http.c` 中添加属性测试：Router dispatch 精确匹配优先
    - **Property 15: Router dispatch exact match priority**
    - **Validates: Requirements 31.5, 31.7**

- [ ] 30. Checkpoint - 确保路由系统编译通过并测试通过

- [ ] 31. 新功能集成测试
  - [ ]* 31.1 在 `tests/test-http.c` 中添加集成测试：服务器使用 chunked 响应 + 客户端接收完整 body、SSE 事件流发送接收、路由 dispatch 端到端、Cookie 跨请求传递、Range 请求 206 响应
    - _Requirements: 25.1-25.6, 26.3-26.8, 27.1-27.6, 29.1-29.6, 31.5-31.7_

- [ ] 32. Final checkpoint - 确保所有新功能测试通过
