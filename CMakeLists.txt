cmake_minimum_required(VERSION 3.30)

project(http_server VERSION 0.1.0 LANGUAGES C)

include(CTest)

find_package(Cups REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(http_server
    src/main.c

    src/http/parser.c
    src/http/request.c
    src/http/response.c

    src/network/socket.c

    src/server/config.c
    src/server/connection.c
    src/server/api.c
    src/server/print_api.c
    src/server/server.c

    src/static/static.c
    src/static/mime_types.c

    src/printing/printing.c
    src/storage/data.c
    src/workers/thread_pool.c

    src/utils/logger.c
    src/utils/resource_monitor.c
)

target_include_directories(http_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CUPS_INCLUDE_DIRS}
)

target_link_libraries(http_server PRIVATE
    ${CUPS_LIBRARIES}
    Threads::Threads
    SQLite3::SQLite3
    OpenSSL::Crypto
)

set_target_properties(http_server PROPERTIES
    C_STANDARD "${CMAKE_C_STANDARD_LATEST}"
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
message(STATUS "Building ${PROJECT_NAME} with C${CMAKE_C_STANDARD_LATEST}")

target_compile_definitions(http_server PRIVATE _POSIX_C_SOURCE=200809L)
if(APPLE)
    target_compile_definitions(http_server PRIVATE _DARWIN_C_SOURCE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(http_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wformat=2
    )
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(test_parser tests/unit/test_parser.c src/http/parser.c)
    add_executable(test_http tests/unit/test_http.c src/http/request.c)
    add_executable(test_buffer tests/unit/test_buffer.c src/http/response.c)
    add_executable(test_router tests/unit/test_router.c src/static/mime_types.c)
    add_executable(test_config tests/unit/test_config.c src/server/config.c)
    add_executable(test_data tests/unit/test_data.c src/storage/data.c)

    foreach(test_target IN ITEMS
            test_parser test_http test_buffer test_router test_config test_data)
        target_include_directories(${test_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(${test_target} PRIVATE _POSIX_C_SOURCE=200809L)
        if(APPLE)
            target_compile_definitions(${test_target} PRIVATE _DARWIN_C_SOURCE)
        endif()
        set_target_properties(${test_target} PROPERTIES
            C_STANDARD "${CMAKE_C_STANDARD_LATEST}"
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${test_target} PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
                -UNDEBUG)
        endif()
    endforeach()

    target_link_libraries(test_data PRIVATE
        Threads::Threads SQLite3::SQLite3 OpenSSL::Crypto)

    add_test(NAME unit_parser COMMAND test_parser)
    add_test(NAME unit_http COMMAND test_http)
    add_test(NAME unit_response COMMAND test_buffer)
    add_test(NAME unit_mime_types COMMAND test_router)
    add_test(NAME unit_config COMMAND test_config)
    add_test(NAME unit_data_store COMMAND test_data)
    add_test(NAME integration_server
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_server.py
            $<TARGET_FILE:http_server>
            ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(integration_server PROPERTIES TIMEOUT 45)
endif()
