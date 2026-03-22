cmake_minimum_required(VERSION 3.16)

project(examples LANGUAGES C)

# TCP examples (always available)
add_executable(tcp-echo-server tcp-echo-server.c)
target_link_libraries(tcp-echo-server PRIVATE xylem)

add_executable(tcp-echo-client tcp-echo-client.c)
target_link_libraries(tcp-echo-client PRIVATE xylem)

# UDP examples (always available)
add_executable(udp-echo-server udp-echo-server.c)
target_link_libraries(udp-echo-server PRIVATE xylem)

add_executable(udp-echo-client udp-echo-client.c)
target_link_libraries(udp-echo-client PRIVATE xylem)

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
endif()
