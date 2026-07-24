cmake_minimum_required(VERSION 3.25)

project(examples LANGUAGES C)

include_directories(${CMAKE_SOURCE_DIR}/src)

# TCP example (always available)
add_executable(example-tcp example-tcp.c)
target_link_libraries(example-tcp PRIVATE xylem)

# Serial example
add_executable(example-serial example-serial.c)
target_link_libraries(example-serial PRIVATE xylem)

