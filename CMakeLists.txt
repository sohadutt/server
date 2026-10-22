cmake_minimum_required(VERSION 3.30)

project(http_server VERSION 0.8.0 LANGUAGES C)

include(CTest)

option(ENABLE_FUZZING "Build libFuzzer parser targets (Clang only)" OFF)

find_package(Cups REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SERVER_SOURCES
    src/main.c
    src/http/parser.c
    src/http/request.c
    src/http/response.c
    src/http/transport.c
    src/network/socket.c
    src/network/buffer.c
    src/network/event_loop.c
    src/router/router.c
    src/server/config.c
    src/server/connection.c
    src/server/api.c
    src/server/print_api.c
    src/server/server.c
    src/security/rate_limit.c
    src/tls/tls.c
    src/static/static.c
    src/static/mime_types.c
    src/printing/printing.c
    src/storage/data.c
    src/workers/thread_pool.c
    src/utils/logger.c
    src/utils/resource_monitor.c
)

function(configure_c_target target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    if(APPLE)
        target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
    endif()
    set_target_properties(${target} PROPERTIES
        C_STANDARD "${CMAKE_C_STANDARD_LATEST}"
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
        )
    endif()
endfunction()

add_executable(http_server ${SERVER_SOURCES})
configure_c_target(http_server)
target_include_directories(http_server PRIVATE ${CUPS_INCLUDE_DIRS})
target_link_libraries(http_server PRIVATE
    ${CUPS_LIBRARIES}
    Threads::Threads
    SQLite3::SQLite3
    OpenSSL::Crypto
    OpenSSL::SSL
)

message(STATUS "Building ${PROJECT_NAME} with C${CMAKE_C_STANDARD_LATEST}")

if(ENABLE_FUZZING)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_FUZZING requires Clang/libFuzzer")
    endif()
    add_executable(fuzz_http_parser
        tests/fuzz/fuzz_http_parser.c src/http/parser.c)
    configure_c_target(fuzz_http_parser)
    target_compile_options(fuzz_http_parser PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_http_parser PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    function(add_unit_test target test_name)
        add_executable(${target} ${ARGN})
        configure_c_target(${target})
        target_compile_options(${target} PRIVATE -UNDEBUG)
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    add_unit_test(test_parser unit_parser
        tests/unit/test_parser.c src/http/parser.c)
    add_unit_test(test_http unit_http
        tests/unit/test_http.c src/http/request.c src/http/transport.c)
    target_link_libraries(test_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_unit_test(test_buffer unit_response
        tests/unit/test_buffer.c src/http/response.c src/http/transport.c
        src/network/buffer.c)
    target_link_libraries(test_buffer PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_unit_test(test_router unit_router
        tests/unit/test_router.c src/router/router.c)
    add_unit_test(test_mime_types unit_mime_types
        tests/unit/test_mime_types.c src/static/mime_types.c)
    add_unit_test(test_config unit_config
        tests/unit/test_config.c src/server/config.c)
    add_unit_test(test_data unit_data_store
        tests/unit/test_data.c src/storage/data.c)
    target_link_libraries(test_data PRIVATE
        Threads::Threads SQLite3::SQLite3 OpenSSL::Crypto)
    add_unit_test(test_event_loop unit_event_loop
        tests/unit/test_event_loop.c src/network/event_loop.c)
    add_unit_test(test_thread_pool unit_thread_pool
        tests/unit/test_thread_pool.c src/workers/thread_pool.c)
    target_link_libraries(test_thread_pool PRIVATE Threads::Threads)
    add_unit_test(test_rate_limit unit_rate_limit
        tests/unit/test_rate_limit.c src/security/rate_limit.c)
    target_link_libraries(test_rate_limit PRIVATE Threads::Threads)
    add_unit_test(test_static unit_static_files
        tests/unit/test_static.c src/static/static.c src/static/mime_types.c
        src/server/config.c src/utils/logger.c)
    target_link_libraries(test_static PRIVATE Threads::Threads)

    add_test(NAME integration_server
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py
            $<TARGET_FILE:http_server>
            ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(integration_server PROPERTIES TIMEOUT 45)
endif()
