cmake_minimum_required(VERSION 3.16)

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

if(WIN32)
	add_compile_options("/experimental:c11atomics")
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/yyjson
)

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
	src/xylem-json.c
	src/yyjson/yyjson.c
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
	find_package(OpenSSL REQUIRED)
	list(APPEND SRCS
		src/xylem-tls.c
		src/xylem-dtls.c
	)
	include_directories(${OPENSSL_INCLUDE_DIR})
endif()

if(XYLEM_ENABLE_DYNAMIC_LIBRARY)
    add_library(xylem SHARED ${SRCS})
else()
    add_library(xylem STATIC ${SRCS})
endif()

target_link_libraries(xylem PRIVATE ${CMAKE_DL_LIBS})

xylem_apply_sanitizer(xylem XYLEM_ENABLE_ASAN address)
xylem_apply_sanitizer(xylem XYLEM_ENABLE_TSAN thread)
xylem_apply_sanitizer(xylem XYLEM_ENABLE_UBSAN undefined)

if(XYLEM_ENABLE_COVERAGE AND UNIX)
    target_compile_options(xylem PRIVATE --coverage)
    target_link_options(xylem PRIVATE --coverage)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include DESTINATION . FILES_MATCHING PATTERN "*")
install(TARGETS xylem DESTINATION lib)

add_subdirectory(examples)

if(XYLEM_ENABLE_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()
