cmake_minimum_required(VERSION 3.25)

project(Xylem LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(xylem-utils)

option(XYLEM_ENABLE_TESTING "enable unit testing" OFF)
option(XYLEM_ENABLE_ASAN "enable memory error detection" OFF)
option(XYLEM_ENABLE_TSAN  "enable data race detection" OFF)
option(XYLEM_ENABLE_UBSAN "enable undefined behavior detection" OFF)
option(XYLEM_ENABLE_DYNAMIC_LIBRARY "build dynamic library" OFF)
option(XYLEM_ENABLE_COVERAGE "enable code coverage reporting" OFF)
option(XYLEM_ENABLE_TLS "enable TLS/DTLS/HTTPS/WSS support (requires OpenSSL)" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install")
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)

if(WIN32 AND MSVC)
	add_compile_options("/experimental:c11atomics")
endif()

# clang-cl's ASan cannot link the debug CRT -- it errors with "doesn't support
# linking with debug runtime libraries yet". When Xylem is built standalone
# with ASan under clang-cl, force the release dynamic CRT (/MD) so a Debug
# configuration still compiles. Guarded by PROJECT_IS_TOP_LEVEL so a consumer
# that does add_subdirectory(xylem) keeps full control of its own CRT choice.
# MSVC cl.exe has no such limitation and is left untouched.
if(PROJECT_IS_TOP_LEVEL AND WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
   AND XYLEM_ENABLE_ASAN)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

set(SRCS
	src/xylem.c
	src/runtime/runtime.c
	src/xylem-logger.c
	src/xylem-utils.c
	src/xylem-serial.c
	src/xylem-timer.c
	src/xylem-ticker.c
	src/container/mpsc.c
	src/thrdpool.c
	src/runtime/wsq.c
	src/runtime/runq.c
	src/runtime/dynpool.c
	src/runtime/scheduler.c
	src/runtime/iowait.c
	src/sync/xylem-waitgroup.c
	src/sync/xylem-channel.c
	src/sync/xylem-mutex.c
	src/sync/xylem-cond.c
	src/sync/xylem-sem.c
	src/sync/tls-wake.c
	src/sync/spin.c
	src/crypto/xylem-sha1.c
	src/crypto/xylem-sha256.c
	src/crypto/xylem-hmac256.c
	src/crypto/aes256/tiny-AES-c/aes.c
	src/crypto/aes256/xylem-aes256.c
	src/encoding/xylem-base64.c
	src/encoding/xylem-bswap.c
	src/encoding/xylem-url.c
	src/encoding/xylem-varint.c
	src/container/heap.c
	src/container/list.c
	src/container/queue.c
	src/container/rbtree.c
	src/container/lifo.c
	src/container/spsc.c
	src/container/xylem-ringbuf.c
	src/container/xylem-heap.c
	src/container/xylem-list.c
	src/container/xylem-queue.c
	src/container/xylem-rbtree.c
	src/container/xylem-stack.c
	src/net/addr.c
	src/net/datagram.c
	src/net/stream.c
	src/net/xylem-tcp.c
	src/net/xylem-udp.c
	src/net/xylem-uds.c
	src/net/xylem-reader.c
	src/net/xylem-writer.c
	src/net/mux/mux-frame.c
	src/net/mux/mux-stream.c
	src/net/mux/xylem-mux.c
	src/net/rudp/xylem-rudp.c
	src/net/rudp/rudp-fec.c
	src/net/rudp/kcp/ikcp.c
	src/encoding/json/xylem-json.c
	src/encoding/json/yyjson/yyjson.c
	src/encoding/gzip/xylem-gzip.c
	src/encoding/gzip/miniz/miniz.c
	src/encoding/fec/reedsolomon-c/rs.c
	src/encoding/fec/xylem-fec.c
	src/net/http/http-utils.c
	src/net/http/http-tunnel.c
	src/net/http/http.c
	src/net/http/xylem-http.c
	src/net/http/http-transport-tcp.c
	src/net/http/xylem-http-auth.c
	src/net/http/xylem-http-cookie.c
	src/net/http/xylem-http-form.c
	src/net/http/xylem-http-multipart.c
	src/net/http/xylem-http-router.c
	src/net/http/xylem-http-cors.c
	src/net/http/xylem-http-fileserver.c
	src/net/http/llhttp/llhttp.c
	src/net/http/llhttp/http.c
	src/net/http/llhttp/api.c
	src/net/ws/ws.c
	src/net/ws/xylem-ws.c
	src/net/ws/ws-tcp.c
	src/net/ws/ws-utf8.c
	src/net/ws/ws-frame.c
	src/net/ws/ws-handshake.c
	src/net/ws/ws-deflate.c
)

if(WIN32)
	list(APPEND SRCS
		src/platform/win/platform-info.c
		src/platform/win/platform-io.c
		src/platform/win/platform-poller.c
		src/platform/win/platform-socket.c
		src/platform/win/platform-sem.c
		src/platform/win/platform-futex.c
		src/platform/win/platform-serial.c
		src/platform/win/platform-vmem.c
		src/platform/win/wepoll/wepoll.c
	)
endif()

if(UNIX)
	list(APPEND SRCS
		src/platform/unix/platform-info.c
		src/platform/unix/platform-io.c
		src/platform/unix/platform-poller.c
		src/platform/unix/platform-socket.c
		src/platform/unix/platform-sem.c
		src/platform/unix/platform-futex.c
		src/platform/unix/platform-serial.c
		src/platform/unix/platform-vmem.c
	)
endif()

if(XYLEM_ENABLE_TLS)
	find_package(OpenSSL 3.5 REQUIRED)
	set_target_properties(OpenSSL::SSL PROPERTIES SYSTEM OFF)
	set_target_properties(OpenSSL::Crypto PROPERTIES SYSTEM OFF)
	list(APPEND SRCS src/net/tls/tls-backend-openssl.c)

	list(APPEND SRCS
		src/net/tls/tls.c
		src/net/tls/xylem-tls.c
		src/net/tls/xylem-dtls.c
		src/net/http/http-transport-tls.c
		src/net/ws/ws-tls.c
	)
else()
	list(APPEND SRCS
		src/net/http/http-transport-tls-stub.c
		src/net/tls/xylem-tls-stub.c
		src/net/ws/ws-tls-stub.c
	)
endif()

if(XYLEM_ENABLE_DYNAMIC_LIBRARY)
    add_library(xylem SHARED ${SRCS})
else()
    add_library(xylem STATIC ${SRCS})
endif()

target_include_directories(xylem
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(xylem PRIVATE ${CMAKE_DL_LIBS})

if(XYLEM_ENABLE_TLS)
	target_link_libraries(xylem PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

xylem_apply_sanitizer(xylem XYLEM_ENABLE_ASAN address)
xylem_apply_sanitizer(xylem XYLEM_ENABLE_TSAN thread)
xylem_apply_sanitizer(xylem XYLEM_ENABLE_UBSAN undefined)

if(XYLEM_ENABLE_COVERAGE AND UNIX)
    target_compile_options(xylem PRIVATE --coverage)
    target_link_options(xylem PUBLIC --coverage)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include DESTINATION . FILES_MATCHING PATTERN "*")
install(TARGETS xylem DESTINATION lib)

add_subdirectory(examples)

if(XYLEM_ENABLE_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()
