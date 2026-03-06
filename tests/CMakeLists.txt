cmake_minimum_required(VERSION 3.16)

project(tests LANGUAGES C)

include(xylem-utils)

xylem_add_test(base64)
xylem_add_test(bswap)
xylem_add_test(heap)
xylem_add_test(list)
xylem_add_test(logger)
xylem_add_test(loop)
xylem_add_test(platform-info)
xylem_add_test(platform-io)
xylem_add_test(platform-poller)
xylem_add_test(platform-socket)
xylem_add_test(queue)
xylem_add_test(rbtree)
xylem_add_test(ringbuf)
xylem_add_test(sha1)
xylem_add_test(sha256)
xylem_add_test(stack)
xylem_add_test(thrdpool)
xylem_add_test(utils)
xylem_add_test(varint)
xylem_add_test(waitgroup)
xylem_add_test(xheap)
xylem_add_test(xlist)
xylem_add_test(xqueue)
xylem_add_test(xrbtree)
xylem_add_test(xstack)

if(XYLEM_ENABLE_COVERAGE AND WIN32)
    find_program(OPENCPPCOVERAGE_BIN OpenCppCoverage)
    if(OPENCPPCOVERAGE_BIN)
        file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/src" SRC_DIR_NATIVE)
        file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/tests" TESTS_DIR_NATIVE)
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/coverage" COVERAGE_OUTPUT_DIR_NATIVE)

        add_custom_target(coverage
            COMMAND ${OPENCPPCOVERAGE_BIN}
                "--sources=${SRC_DIR_NATIVE}"
                "--excluded_sources=${TESTS_DIR_NATIVE}"
                "--export_type=html:${COVERAGE_OUTPUT_DIR_NATIVE}"
                "--cover_children"
                "--"
                ctest.exe --test-dir "${CMAKE_BINARY_DIR}" -C $<CONFIG> --output-on-failure
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "generating HTML coverage report in coverage/index.html"
            USES_TERMINAL
        )
    else()
        message(WARNING "OpenCppCoverage not found. install it and add to PATH.")
    endif()
endif()

if(XYLEM_ENABLE_COVERAGE AND UNIX)
    find_program(LCOV_BIN lcov)
    find_program(GENHTML_BIN genhtml)
    if(LCOV_BIN AND GENHTML_BIN)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
            COMMAND ctest --test-dir ${CMAKE_BINARY_DIR} -C $<CONFIG> --output-on-failure
            COMMAND ${LCOV_BIN} --capture --directory . --output-file ${CMAKE_BINARY_DIR}/coverage/coverage.info
            COMMAND ${GENHTML_BIN} ${CMAKE_BINARY_DIR}/coverage/coverage.info --output-directory ${CMAKE_BINARY_DIR}/coverage/html
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "generating html coverage report in coverage/html/index.html"
        )
    else()
        message(WARNING "lcov or genhtml not found; 'coverage' target not created.")
    endif()
endif()