cmake_minimum_required(VERSION 3.25)

project(examples LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/src)

# TCP example (always available)
add_executable(tcp-echo example-tcp.c)
target_link_libraries(tcp-echo PRIVATE xylem)

# Serial example (disabled -- uses thrd_t directly, needs C11 threads header)
# add_executable(serial-terminal example-serial.c)
# target_link_libraries(serial-terminal PRIVATE xylem)

