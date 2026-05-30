cmake_minimum_required(VERSION 3.25)

project(examples LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/src)

# TCP example (always available)
add_executable(tcp-echo tcp-echo.c)
target_link_libraries(tcp-echo PRIVATE xylem)

# HTTP examples (disabled: pending port to coroutine-based API)
# if(XYLEM_ENABLE_HTTP)
#     add_executable(http-session-client http-session-client.c)
#     target_link_libraries(http-session-client PRIVATE xylem)
# endif()

# Serial example (disabled -- uses thrd_t directly, needs C11 threads header)
# add_executable(serial-terminal serial-terminal.c)
# target_link_libraries(serial-terminal PRIVATE xylem)

