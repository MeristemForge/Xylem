cmake_minimum_required(VERSION 3.25)

project(examples LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/src)

# TCP example (always available)
add_executable(tcp-echo tcp-echo.c)
target_link_libraries(tcp-echo PRIVATE xylem)

# HTTP examples
if(XYLEM_ENABLE_HTTP)
    add_executable(http-static-server http-static-server.c)
    target_link_libraries(http-static-server PRIVATE xylem)

    add_executable(http-session-client http-session-client.c)
    target_link_libraries(http-session-client PRIVATE xylem)
endif()

# Serial example (disabled -- uses thrd_t directly, needs C11 threads header)
# add_executable(serial-terminal serial-terminal.c)
# target_link_libraries(serial-terminal PRIVATE xylem)

# TLS examples (only when TLS is enabled)
if(XYLEM_ENABLE_TLS)
    add_executable(https-static-server https-static-server.c)
    target_link_libraries(https-static-server PRIVATE xylem OpenSSL::SSL OpenSSL::Crypto)
endif()
