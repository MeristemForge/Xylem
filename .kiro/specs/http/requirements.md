# Requirements Document

## Introduction

本模块为 Xylem C 库添加 HTTP/1.1 支持，包含同步客户端 API 和异步服务器 API。HTTP 解析基于 llhttp（源码集成，与 yyjson 模式一致，放置于 `src/http/llhttp/`）。客户端 API 对外表现为同步阻塞调用，内部通过创建临时 xylem_loop 实现异步 TCP/TLS 操作；服务器 API 运行在用户提供的 event loop 上，通过回调处理请求。模块自动处理 URL 解析、DNS 解析、HTTP 请求序列化、响应解析、Header 自动填充及 chunked transfer encoding。

## Glossary

- **HTTP_Client**: Xylem HTTP 同步客户端，提供 `xylem_http_get`、`xylem_http_post`、`xylem_http_post_json` 等函数，内部创建临时 event loop 执行异步网络操作并阻塞至完成
- **HTTP_Server**: Xylem HTTP 异步服务器，运行在用户提供的 `xylem_loop_t` 上，通过 `on_request` 回调将请求分发给用户处理
- **HTTP_Response**: 客户端收到的 HTTP 响应对象（`xylem_http_res_t`），包含状态码、响应头和响应体，为不透明类型，使用 create/destroy 生命周期
- **HTTP_Request**: 服务器端收到的 HTTP 请求对象（`xylem_http_req_t`），包含方法、URL、请求头和请求体，在 `on_request` 回调中有效
- **HTTP_Connection**: 服务器端的客户端连接对象（`xylem_http_conn_t`），用于在回调中发送响应和关闭连接
- **HTTP_Server_Config**: 服务器配置结构体（`xylem_http_srv_cfg_t`），包含 host、port、回调函数、userdata 及可选的 TLS 证书/密钥路径
- **URL_Parser**: 内部 URL 解析组件，从 URL 字符串中提取 scheme、host、port、path
- **llhttp**: Node.js 官方 HTTP 解析器，TypeScript 生成的 C 代码，用于解析 HTTP 请求和响应消息
- **Request_Serializer**: 内部组件，将 HTTP 方法、URL、Headers 和 Body 序列化为符合 HTTP/1.1 规范的请求报文

## Requirements

### Requirement 1: llhttp 源码集成

**User Story:** 作为开发者，我希望 llhttp 以源码形式集成到项目中，以便无需外部依赖即可解析 HTTP 消息。

#### Acceptance Criteria

1. THE HTTP_Client SHALL include llhttp source files (llhttp.c, http.c, api.c) and header (llhttp.h) in the `src/http/llhttp/` directory
2. WHEN the project is built, THE build system SHALL compile llhttp source files as part of the xylem library without requiring external dependencies
3. THE build system SHALL add `src/http/llhttp/` to the include path so that internal source files can include `llhttp.h`

### Requirement 2: URL 解析

**User Story:** 作为开发者，我希望客户端能自动解析 URL，以便我只需传入完整 URL 字符串即可发起请求。

#### Acceptance Criteria

1. WHEN a valid HTTP URL is provided, THE URL_Parser SHALL extract the scheme, host, port, and path components
2. WHEN the URL scheme is "http", THE URL_Parser SHALL default the port to 80 if no port is specified
3. WHEN the URL scheme is "https", THE URL_Parser SHALL default the port to 443 if no port is specified
4. WHEN the URL contains an explicit port (e.g. `:8080`), THE URL_Parser SHALL use the specified port instead of the default
5. WHEN the URL does not contain a path component, THE URL_Parser SHALL default the path to "/"
6. IF an invalid or unsupported URL is provided, THEN THE URL_Parser SHALL return an error code of -1
7. THE URL_Parser SHALL serialize a parsed URL back into a valid URL string
8. FOR ALL valid URL strings, parsing then serializing then parsing SHALL produce an equivalent parsed result (round-trip property)

### Requirement 3: HTTP 请求序列化

**User Story:** 作为开发者，我希望客户端能自动构建符合 HTTP/1.1 规范的请求报文，以便我无需手动拼接 HTTP 协议文本。

#### Acceptance Criteria

1. WHEN a request is serialized, THE Request_Serializer SHALL produce a message conforming to HTTP/1.1 format: request-line, headers, empty line, and optional body
2. THE Request_Serializer SHALL automatically set the Host header from the URL host and port
3. WHEN a request body is provided, THE Request_Serializer SHALL automatically set the Content-Length header to the body length in bytes
4. THE Request_Serializer SHALL set the Connection header to "keep-alive" by default
5. THE Request_Serializer SHALL format a serialized HTTP request back into its component parts (method, URL, headers, body)
6. FOR ALL valid request components, serializing then parsing via llhttp SHALL produce equivalent components (round-trip property)

### Requirement 4: HTTP 响应解析

**User Story:** 作为开发者，我希望客户端能自动解析服务器返回的 HTTP 响应，以便我可以直接读取状态码、响应头和响应体。

#### Acceptance Criteria

1. WHEN a complete HTTP response is received, THE HTTP_Client SHALL parse the status code, headers, and body using llhttp
2. WHEN the response uses Content-Length, THE HTTP_Client SHALL read exactly the specified number of body bytes
3. WHEN the response uses chunked transfer encoding, THE HTTP_Client SHALL reassemble the chunks into a contiguous body buffer
4. WHEN the response contains multiple headers with the same name, THE HTTP_Client SHALL preserve all header values
5. IF the response is malformed or llhttp reports a parse error, THEN THE HTTP_Client SHALL return NULL from the request function

### Requirement 5: 同步 GET 请求

**User Story:** 作为开发者，我希望通过一个简单的函数调用发起 HTTP GET 请求并获取响应，以便快速获取远程资源。

#### Acceptance Criteria

1. WHEN `xylem_http_get` is called with a valid URL, THE HTTP_Client SHALL create a temporary event loop, resolve the host via xylem_addr, establish a TCP connection, send the GET request, receive and parse the response, and return an HTTP_Response pointer
2. WHEN the URL scheme is "http", THE HTTP_Client SHALL use xylem_tcp for the connection
3. WHEN the URL scheme is "https", THE HTTP_Client SHALL use xylem_tls for the connection
4. THE HTTP_Client SHALL block the calling thread until the response is fully received or an error occurs
5. IF DNS resolution fails, THEN THE HTTP_Client SHALL return NULL
6. IF the TCP or TLS connection fails, THEN THE HTTP_Client SHALL return NULL
7. IF a network timeout occurs, THEN THE HTTP_Client SHALL return NULL

### Requirement 6: 同步 POST 请求

**User Story:** 作为开发者，我希望通过简单的函数调用发起 HTTP POST 请求，以便向服务器提交数据。

#### Acceptance Criteria

1. WHEN `xylem_http_post` is called with a URL, body, body length, and content type, THE HTTP_Client SHALL send a POST request with the specified body and Content-Type header, and return an HTTP_Response pointer
2. WHEN `xylem_http_post_json` is called with a URL and JSON string, THE HTTP_Client SHALL send a POST request with Content-Type set to "application/json" and return an HTTP_Response pointer
3. WHEN a POST request body is provided, THE HTTP_Client SHALL set the Content-Length header to the body length
4. IF the body pointer is NULL and body length is 0, THEN THE HTTP_Client SHALL send a POST request with an empty body and Content-Length of 0

### Requirement 7: 响应对象读取与销毁

**User Story:** 作为开发者，我希望通过访问器函数读取响应的各个字段，并在使用完毕后释放资源。

#### Acceptance Criteria

1. THE HTTP_Response SHALL provide `xylem_http_res_status` to return the HTTP status code as an int
2. THE HTTP_Response SHALL provide `xylem_http_res_header` to return the value of a specified header name, using case-insensitive matching
3. WHEN the requested header does not exist, THE HTTP_Response SHALL return NULL from `xylem_http_res_header`
4. THE HTTP_Response SHALL provide `xylem_http_res_body` to return a pointer to the response body data
5. THE HTTP_Response SHALL provide `xylem_http_res_body_len` to return the response body length in bytes
6. WHEN `xylem_http_res_destroy` is called, THE HTTP_Response SHALL free all memory associated with the response object
7. WHEN `xylem_http_res_destroy` is called with NULL, THE HTTP_Response SHALL perform no operation

### Requirement 8: HTTP 服务器创建与生命周期

**User Story:** 作为开发者，我希望创建一个异步 HTTP 服务器并控制其启动和停止，以便在我的 event loop 中处理 HTTP 请求。

#### Acceptance Criteria

1. WHEN `xylem_http_srv_create` is called with a valid loop and config, THE HTTP_Server SHALL allocate and return a server handle
2. THE HTTP_Server SHALL accept the event loop as a separate parameter, not as part of the config struct
3. THE HTTP_Server_Config SHALL contain host, port, on_request callback, userdata, and optional tls_cert/tls_key fields
4. WHEN `xylem_http_srv_start` is called, THE HTTP_Server SHALL bind to the configured host and port and begin accepting connections
5. WHEN tls_cert and tls_key are both NULL in the config, THE HTTP_Server SHALL accept plain HTTP connections using xylem_tcp
6. WHEN tls_cert and tls_key are both set in the config, THE HTTP_Server SHALL accept HTTPS connections using xylem_tls
7. WHEN `xylem_http_srv_stop` is called, THE HTTP_Server SHALL stop accepting new connections while existing connections continue processing
8. WHEN `xylem_http_srv_destroy` is called, THE HTTP_Server SHALL free all resources associated with the server handle
9. IF `xylem_http_srv_create` receives a NULL loop or NULL config, THEN THE HTTP_Server SHALL return NULL
10. IF binding to the configured host and port fails, THEN THE HTTP_Server SHALL return -1 from `xylem_http_srv_start`

### Requirement 9: 服务器请求处理

**User Story:** 作为开发者，我希望在回调中读取客户端请求的所有信息，以便根据请求内容生成响应。

#### Acceptance Criteria

1. WHEN a complete HTTP request is received, THE HTTP_Server SHALL invoke the on_request callback with the connection handle, request object, and userdata
2. THE HTTP_Request SHALL provide `xylem_http_req_method` to return the request method string (e.g. "GET", "POST")
3. THE HTTP_Request SHALL provide `xylem_http_req_url` to return the request URL path string
4. THE HTTP_Request SHALL provide `xylem_http_req_header` to return the value of a specified header name, using case-insensitive matching
5. WHEN the requested header does not exist, THE HTTP_Request SHALL return NULL from `xylem_http_req_header`
6. THE HTTP_Request SHALL provide `xylem_http_req_body` to return a pointer to the request body data
7. THE HTTP_Request SHALL provide `xylem_http_req_body_len` to return the request body length in bytes
8. WHEN the request has no body, THE HTTP_Request SHALL return NULL from `xylem_http_req_body` and 0 from `xylem_http_req_body_len`

### Requirement 10: 服务器响应发送

**User Story:** 作为开发者，我希望在回调中向客户端发送 HTTP 响应，以便完成请求-响应周期。

#### Acceptance Criteria

1. WHEN `xylem_http_conn_send` is called with a status code, content type, body, and body length, THE HTTP_Connection SHALL serialize and send a complete HTTP/1.1 response including status line, Content-Type header, Content-Length header, and body
2. WHEN body is NULL and body length is 0, THE HTTP_Connection SHALL send a response with no body and Content-Length of 0
3. WHEN `xylem_http_conn_close` is called, THE HTTP_Connection SHALL close the underlying TCP or TLS connection
4. THE HTTP_Connection SHALL return 0 from `xylem_http_conn_send` on success and -1 on failure
5. IF `xylem_http_conn_send` is called after the connection has been closed, THEN THE HTTP_Connection SHALL return -1

### Requirement 11: Header 大小写不敏感匹配

**User Story:** 作为开发者，我希望按名称查找 HTTP Header 时不区分大小写，以便符合 HTTP/1.1 规范（RFC 7230 §3.2）。

#### Acceptance Criteria

1. WHEN searching for a header by name, THE HTTP_Response SHALL perform case-insensitive ASCII comparison
2. WHEN searching for a header by name, THE HTTP_Request SHALL perform case-insensitive ASCII comparison
3. WHEN a header name "Content-Type" is queried as "content-type", THE HTTP_Response SHALL return the matching header value

### Requirement 12: 模块文件结构与构建集成

**User Story:** 作为开发者，我希望 HTTP 模块遵循 Xylem 项目的文件组织和命名规范，以便与现有模块保持一致。

#### Acceptance Criteria

1. THE HTTP_Client SHALL have its public API declared in `include/xylem/xylem-http.h`
2. THE HTTP_Client SHALL have its implementation in `src/xylem-http.c`
3. THE build system SHALL add `src/xylem-http.c` and the llhttp source files to the SRCS list in the root CMakeLists.txt
4. THE build system SHALL add `src/http/llhttp/` to the include directories
5. THE HTTP_Client SHALL have a test file at `tests/test-http.c` registered via `xylem_add_test(http)` in `tests/CMakeLists.txt`
6. THE umbrella header `include/xylem.h` SHALL include `xylem/xylem-http.h`
7. THE HTTP_Client SHALL use opaque types with create/destroy lifecycle for HTTP_Response, and the server SHALL use create/destroy for HTTP_Server


### Requirement 13: Connection 管理与 keep-alive

**User Story:** 作为开发者，我希望 HTTP 连接能正确处理 keep-alive 和 Connection: close 语义，以便符合 HTTP/1.1 规范。

#### Acceptance Criteria

1. WHEN the HTTP_Client receives a response with `Connection: close` header, THE HTTP_Client SHALL close the TCP/TLS connection after reading the response
2. WHEN the HTTP_Server receives a request with `Connection: close` header, THE HTTP_Server SHALL close the connection after sending the response
3. WHEN the HTTP_Server completes a response and the connection is keep-alive, THE HTTP_Server SHALL continue reading the next request on the same connection
4. THE HTTP_Server SHALL support processing multiple sequential requests on a single keep-alive connection
5. WHEN the HTTP_Server sends a response with `Connection: close`, THE HTTP_Server SHALL close the connection after the response is fully sent

### Requirement 14: 客户端超时配置

**User Story:** 作为开发者，我希望能配置客户端请求的超时时间，以便避免请求无限期阻塞。

#### Acceptance Criteria

1. THE HTTP_Client SHALL provide `xylem_http_set_timeout` to configure a default timeout in milliseconds for all subsequent requests
2. WHEN a timeout is configured and the request does not complete within the specified duration, THE HTTP_Client SHALL abort the request, clean up resources, and return NULL
3. WHEN the timeout is set to 0, THE HTTP_Client SHALL disable the timeout and wait indefinitely
4. THE HTTP_Client SHALL apply the timeout to the entire request lifecycle including DNS resolution, connection establishment, request sending, and response receiving
5. IF `xylem_http_set_timeout` is not called, THE HTTP_Client SHALL use a default timeout of 30000 milliseconds (30 seconds)

### Requirement 15: 状态码到 Reason Phrase 映射

**User Story:** 作为开发者，我希望服务器响应序列化时能自动填充标准的 reason phrase，以便生成符合规范的 HTTP 响应。

#### Acceptance Criteria

1. WHEN the HTTP_Server serializes a response, THE HTTP_Server SHALL include the standard reason phrase for the given status code in the status line (e.g. "HTTP/1.1 200 OK", "HTTP/1.1 404 Not Found")
2. THE HTTP_Server SHALL support reason phrases for all standard HTTP/1.1 status codes (1xx, 2xx, 3xx, 4xx, 5xx)
3. WHEN an unrecognized status code is provided, THE HTTP_Server SHALL use an empty reason phrase

### Requirement 16: 客户端重定向处理

**User Story:** 作为开发者，我希望客户端默认不自动跟随重定向，但提供选项启用自动重定向，以便灵活控制重定向行为。

#### Acceptance Criteria

1. WHEN the HTTP_Client receives a 301, 302, 307, or 308 response, THE HTTP_Client SHALL by default return the redirect response as-is without following the redirect
2. THE HTTP_Client SHALL provide `xylem_http_set_follow_redirects` to enable automatic redirect following with a maximum redirect count
3. WHEN automatic redirect following is enabled and a redirect response is received, THE HTTP_Client SHALL extract the Location header and issue a new request to the redirect target
4. WHEN the redirect count exceeds the configured maximum, THE HTTP_Client SHALL return the last redirect response instead of following further
5. WHEN a 307 or 308 redirect is followed, THE HTTP_Client SHALL preserve the original HTTP method and body
6. WHEN a 301 or 302 redirect is followed, THE HTTP_Client SHALL use GET method for the redirected request regardless of the original method

### Requirement 17: 其他 HTTP 方法支持

**User Story:** 作为开发者，我希望客户端支持 PUT、DELETE、PATCH 等常用 HTTP 方法，以便与 RESTful API 交互。

#### Acceptance Criteria

1. THE HTTP_Client SHALL provide `xylem_http_put` to send a PUT request with a URL, body, body length, and content type
2. THE HTTP_Client SHALL provide `xylem_http_delete` to send a DELETE request with a URL
3. THE HTTP_Client SHALL provide `xylem_http_patch` to send a PATCH request with a URL, body, body length, and content type
4. THE HTTP_Client SHALL set the Content-Length header automatically for PUT and PATCH requests when a body is provided

### Requirement 18: 请求/响应 Body 大小限制

**User Story:** 作为开发者，我希望能限制请求和响应 body 的最大大小，以便防止内存耗尽。

#### Acceptance Criteria

1. THE HTTP_Server SHALL enforce a configurable maximum request body size, defaulting to 1 MiB (1048576 bytes)
2. WHEN a request body exceeds the configured maximum, THE HTTP_Server SHALL respond with 413 Payload Too Large and close the connection
3. THE HTTP_Client SHALL enforce a configurable maximum response body size, defaulting to 10 MiB (10485760 bytes)
4. WHEN a response body exceeds the configured maximum, THE HTTP_Client SHALL abort the request, clean up resources, and return NULL
5. THE HTTP_Server_Config SHALL provide a `max_body_size` field to configure the server-side limit
6. THE HTTP_Client SHALL provide `xylem_http_set_max_body_size` to configure the client-side limit

### Requirement 19: 服务端连接超时

**User Story:** 作为开发者，我希望服务器能自动关闭空闲连接，以便释放资源。

#### Acceptance Criteria

1. THE HTTP_Server SHALL close a keep-alive connection that has been idle (no new request received) for longer than the configured idle timeout
2. THE HTTP_Server_Config SHALL provide an `idle_timeout_ms` field to configure the idle timeout, defaulting to 60000 milliseconds (60 seconds)
3. WHEN the idle timeout is set to 0, THE HTTP_Server SHALL disable idle timeout and keep connections open indefinitely
4. THE HTTP_Server SHALL reset the idle timer each time a new request is received on the connection

### Requirement 20: 100-Continue 处理

**User Story:** 作为开发者，我希望客户端在发送大 body 前能等待服务端确认，以便避免不必要的数据传输。

#### Acceptance Criteria

1. WHEN the HTTP_Client sends a request with a body larger than 1024 bytes, THE HTTP_Client SHALL include the `Expect: 100-continue` header and wait for a 100 Continue response before sending the body
2. WHEN the HTTP_Client receives a 100 Continue response, THE HTTP_Client SHALL proceed to send the request body
3. WHEN the HTTP_Client receives a non-100 response (e.g. 417 Expectation Failed) instead of 100 Continue, THE HTTP_Client SHALL return that response without sending the body
4. WHEN the HTTP_Server receives a request with `Expect: 100-continue`, THE HTTP_Server SHALL send a 100 Continue response before invoking the on_request callback
5. IF the HTTP_Client does not receive a 100 Continue response within 1 second, THE HTTP_Client SHALL send the body anyway

### Requirement 21: URL Percent-Encoding

**User Story:** 作为开发者，我希望 URL 中的特殊字符能被正确编码和解码，以便处理包含空格、中文等字符的 URL。

#### Acceptance Criteria

1. WHEN the URL_Parser encounters percent-encoded characters (e.g. `%20`, `%E4%B8%AD`), THE URL_Parser SHALL decode them to their original byte values
2. THE HTTP_Client SHALL provide `xylem_http_url_encode` to percent-encode a string for use in URL path or query components
3. THE HTTP_Client SHALL provide `xylem_http_url_decode` to decode a percent-encoded string
4. THE URL_Parser SHALL preserve percent-encoded characters in the path when serializing the request line
5. FOR ALL valid byte sequences, encoding then decoding SHALL produce the original byte sequence (round-trip property)
