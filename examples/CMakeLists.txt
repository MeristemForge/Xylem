cmake_minimum_required(VERSION 3.25)

project(examples LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/src)

# TCP examples (always available)
add_executable(tcp-echo-server tcp-echo-server.c)
target_link_libraries(tcp-echo-server PRIVATE xylem)

add_executable(tcp-echo-client tcp-echo-client.c)
target_link_libraries(tcp-echo-client PRIVATE xylem)

# HTTP examples
if(XYLEM_ENABLE_HTTP)
    add_executable(http-echo-server http-echo-server.c)
    target_link_libraries(http-echo-server PRIVATE xylem)

    add_executable(http-echo-client http-echo-client.c)
    target_link_libraries(http-echo-client PRIVATE xylem)

    add_executable(http-static-server http-static-server.c)
    target_link_libraries(http-static-server PRIVATE xylem)

    add_executable(http-session-client http-session-client.c)
    target_link_libraries(http-session-client PRIVATE xylem)
endif()

# WebSocket examples
if(XYLEM_ENABLE_WS)
    add_executable(ws-echo-server ws-echo-server.c)
    target_link_libraries(ws-echo-server PRIVATE xylem)

    add_executable(ws-echo-client ws-echo-client.c)
    target_link_libraries(ws-echo-client PRIVATE xylem)
endif()

# Serial example (disabled -- uses thrd_t directly, needs C11 threads header)
# add_executable(serial-terminal serial-terminal.c)
# target_link_libraries(serial-terminal PRIVATE xylem)

# TLS/DTLS examples (only when TLS is enabled)
if(XYLEM_ENABLE_TLS)
    add_executable(tls-echo-server tls-echo-server.c)
    target_link_libraries(tls-echo-server PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(tls-echo-client tls-echo-client.c)
    target_link_libraries(tls-echo-client PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(dtls-echo-server dtls-echo-server.c)
    target_link_libraries(dtls-echo-server PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(dtls-echo-client dtls-echo-client.c)
    target_link_libraries(dtls-echo-client PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(https-echo-server https-echo-server.c)
    target_link_libraries(https-echo-server PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(https-echo-client https-echo-client.c)
    target_link_libraries(https-echo-client PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(https-static-server https-static-server.c)
    target_link_libraries(https-static-server PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(wss-echo-server wss-echo-server.c)
    target_link_libraries(wss-echo-server PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)

    add_executable(wss-echo-client wss-echo-client.c)
    target_link_libraries(wss-echo-client PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)
endif()
