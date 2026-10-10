cmake_minimum_required(VERSION 3.30)

project(http_server VERSION 0.1.0 LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(http_server
    src/main.c

    src/http/parser.c
    src/http/request.c
    src/http/response.c

    src/network/socket.c

    src/server/config.c
    src/server/connection.c
    src/server/server.c

    src/static/static.c

    src/utils/logger.c
    src/utils/resource_monitor.c
)

target_include_directories(http_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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
