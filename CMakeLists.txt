cmake_minimum_required(VERSION 3.16)

project(http_server VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(http_server
    src/main.c

    src/http/http.c
    src/http/parser.c
    src/http/request.c
    src/http/response.c

    src/network/buffer.c
    src/network/event_loop.c
    src/network/socket.c

    src/router/router.c

    src/server/config.c
    src/server/connection.c
    src/server/server.c

    src/static/static.c

    src/tls/tls.c

    src/utils/logger.c
    src/utils/resource_monitor.c

    src/workers/thread_pool.c
)

target_include_directories(http_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
