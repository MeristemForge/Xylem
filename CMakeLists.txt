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
option(XYLEM_ENABLE_TLS "enable TLS/DTLS support (requires OpenSSL)" OFF)
option(XYLEM_ENABLE_HTTP "enable HTTP client/server" OFF)
option(XYLEM_ENABLE_WS "enable WebSocket client/server" OFF)

if(XYLEM_ENABLE_WS AND NOT XYLEM_ENABLE_HTTP)
	set(XYLEM_ENABLE_HTTP ON CACHE BOOL "" FORCE)
endif()

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

set(SRCS
	src/xylem.c
	src/runtime/runtime.c
	src/xylem-logger.c
	src/xylem-utils.c
	src/xylem-serial.c
	src/xylem-timer.c
	src/container/mpsc.c
	src/thrdpool.c
	src/runtime/wsdeque.c
	src/runtime/runq.c
	src/runtime/dynpool.c
	src/runtime/scheduler.c
	src/runtime/iowait.c
	src/sync/xylem-waitgroup.c
	src/sync/xylem-channel.c
	src/sync/xylem-mutex.c
	src/sync/xylem-cond.c
	src/sync/spin.c
	src/crypto/xylem-sha1.c
	src/crypto/xylem-sha256.c
	src/crypto/xylem-hmac256.c
	src/crypto/aes256/tiny-AES-c/aes.c
	src/crypto/aes256/xylem-aes256.c
	src/encoding/xylem-base64.c
	src/encoding/xylem-bswap.c
	src/encoding/xylem-varint.c
	src/container/heap.c
	src/container/list.c
	src/container/queue.c
	src/container/rbtree.c
	src/container/lifo.c
	src/container/xylem-ringbuf.c
	src/container/xylem-heap.c
	src/container/xylem-list.c
	src/container/xylem-queue.c
	src/container/xylem-rbtree.c
	src/container/xylem-stack.c
	src/net/addr.c
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
)

if(WIN32)
	list(APPEND SRCS
		src/platform/win/platform-info.c
		src/platform/win/platform-io.c
		src/platform/win/platform-poller.c
		src/platform/win/platform-socket.c
		src/platform/win/platform-sem.c
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
		src/platform/unix/platform-serial.c
		src/platform/unix/platform-vmem.c
	)
endif()

if(XYLEM_ENABLE_TLS)
	find_package(OpenSSL 3.5 REQUIRED)

	set_target_properties(OpenSSL::SSL PROPERTIES SYSTEM OFF)
	set_target_properties(OpenSSL::Crypto PROPERTIES SYSTEM OFF)

	list(APPEND SRCS
		src/net/xylem-tls.c
		src/net/xylem-dtls.c
	)
endif()

if(XYLEM_ENABLE_HTTP)
	list(APPEND SRCS
		src/net/http/utils.c
		src/net/http/http.c
		src/net/http/xylem-http.c
		src/net/http/llhttp/llhttp.c
		src/net/http/llhttp/http.c
		src/net/http/llhttp/api.c
	)
	if(XYLEM_ENABLE_TLS)
		list(APPEND SRCS src/net/http/xylem-https.c)
	endif()
endif()

if(XYLEM_ENABLE_WS)
	list(APPEND SRCS
		src/net/ws/ws.c
		src/net/ws/xylem-ws.c
		src/net/ws/ws-utf8.c
		src/net/ws/ws-frame.c
		src/net/ws/ws-handshake.c
	)
	if(XYLEM_ENABLE_TLS)
		list(APPEND SRCS src/net/ws/xylem-wss.c)
	endif()
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
	target_compile_definitions(xylem PUBLIC XYLEM_ENABLE_TLS)
	target_link_libraries(xylem PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(XYLEM_ENABLE_HTTP)
	target_compile_definitions(xylem PUBLIC XYLEM_ENABLE_HTTP)
endif()

if(XYLEM_ENABLE_WS)
	target_compile_definitions(xylem PUBLIC XYLEM_ENABLE_WS)
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
