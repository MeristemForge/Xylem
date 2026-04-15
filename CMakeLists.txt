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
	src/xylem-addr.c
	src/xylem-tcp.c
	src/xylem-udp.c
	src/xylem-base64.c
	src/xylem-bswap.c
	src/xylem-heap.c
	src/xylem-list.c
	src/xylem-logger.c
	src/xylem-queue.c
	src/xylem-rbtree.c
	src/xylem-ringbuf.c
	src/xylem-sha1.c
	src/xylem-sha256.c
	src/xylem-hmac256.c
	src/xylem-stack.c
	src/xylem-thrdpool.c
	src/xylem-loop.c
	src/xylem-utils.c
	src/xylem-varint.c
	src/xylem-waitgroup.c
	src/xylem-xheap.c
	src/xylem-xlist.c
	src/xylem-xqueue.c
	src/xylem-xrbtree.c
	src/xylem-xstack.c
	src/json/xylem-json.c
	src/json/yyjson/yyjson.c
	src/http/http-common.c
	src/http/xylem-http-client.c
	src/http/xylem-http-server.c
	src/http/xylem-http-common.c
	src/http/http-transport-tcp.c
	src/http/llhttp/llhttp.c
	src/http/llhttp/http.c
	src/http/llhttp/api.c
	src/gzip/xylem-gzip.c
	src/gzip/miniz/miniz.c
	src/aes256/tiny-AES-c/aes.c
	src/aes256/xylem-aes256.c
	src/ws/ws-common.c
	src/ws/xylem-ws-client.c
	src/ws/xylem-ws-server.c
	src/ws/ws-utf8.c
	src/ws/ws-frame.c
	src/ws/ws-handshake.c
	src/ws/ws-transport-tcp.c
	src/rudp/kcp/ikcp.c
	src/rudp/xylem-rudp.c
	src/rudp/rudp-fec.c
	src/xylem-uds.c
	src/fec/reedsolomon-c/rs.c
	src/fec/xylem-fec.c
)

if(WIN32)
	list(APPEND SRCS
		src/platform/win/platform-info.c
		src/platform/win/platform-io.c
		src/platform/win/platform-poller.c
		src/platform/win/platform-socket.c
		src/platform/win/wepoll/wepoll.c
	)
endif()

if(UNIX)
	list(APPEND SRCS
		src/platform/unix/platform-info.c
		src/platform/unix/platform-io.c
		src/platform/unix/platform-poller.c
		src/platform/unix/platform-socket.c
	)
endif()

if(XYLEM_ENABLE_TLS)
	find_package(OpenSSL 3.5 REQUIRED)

	set_target_properties(OpenSSL::SSL PROPERTIES SYSTEM OFF)
	set_target_properties(OpenSSL::Crypto PROPERTIES SYSTEM OFF)

	list(APPEND SRCS
		src/xylem-tls.c
		src/xylem-dtls.c
		src/http/http-transport-tls.c
		src/ws/ws-transport-tls.c
	)
else()
	list(APPEND SRCS
		src/http/http-transport-tls-stub.c
		src/ws/ws-transport-tls-stub.c
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
