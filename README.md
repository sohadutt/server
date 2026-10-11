# HTTP Server

A small C HTTP server with separate modules for configuration, TCP sockets,
connections, HTTP requests and responses, static files, logging, and resource
monitoring. The server reads up to 4,095 bytes from a client and serves files
from the configured document root.

## Build

Requires CMake 3.30 or newer and a C compiler. CMake selects the latest C
standard it supports with the active compiler, disables compiler-specific C
extensions, and enables strict warnings for Clang and GCC.

```sh
cmake -S . -B build
cmake --build build
./build/http_server
```

Pass a config file path as the first argument to use a different config:

```sh
./build/http_server path/to/server.conf
```

## Server configuration

`config/server.conf` uses one `key = value` setting per line. Blank lines and
lines beginning with `#` are ignored. Missing settings use the defaults below.
`document_root` and `mime_types_file` paths are relative to the process working
directory unless they are absolute paths.

| Setting | Default | Valid values |
| --- | --- | --- |
| `host` | `0.0.0.0` | Host name or IP address |
| `port` | `8080` | `1`–`65535` |
| `worker_threads` | `4` | `1`–`1024` |
| `document_root` | `public` | Existing directory |
| `index_file` | `index.html` | Relative file inside document root |
| `not_found_page` | `404.html` | Relative file inside document root |
| `mime_types_file` | `config/mime.types` | MIME mapping file |
| `default_mime_type` | `application/octet-stream` | Fallback media type |
| `max_static_file_size_mb` | `64` | `1`–`1024` |
| `log_level` | `info` | `debug`, `info`, `warn`, `error` |
| `log_console` | `true` | `true`, `false`, `1`, `0` |
| `log_file` | `true` | `true`, `false`, `1`, `0` |
| `log_directory` | `logs` | Non-empty path |

Unknown settings and invalid values stop startup with the config filename and
line number.

With `log_level = debug`, each handled connection also logs request timing,
CPU time, page faults, context switches, bytes received and sent, and the
server process's peak resident memory.

## Static file selection

Only `GET` requests are served. The URL path maps to a file below
`document_root`: `/` selects `index_file`, `/about.html` selects
`document_root/about.html`, and `/assets/app.js` selects
`document_root/assets/app.js`. A directory URL selects the configured
`index_file` within that directory. Query strings do not affect file selection.
Paths that try to leave the document root are rejected. Missing files return
the configured `not_found_page` with a `404` status.

`mime_types_file` uses one `extension media/type` pair per line, with blank
lines and `#` comments allowed. For example, `js text/javascript` makes `.js`
files use `Content-Type: text/javascript`. Extensions are matched without
case sensitivity. Files with an unmapped extension use `default_mime_type`.
The configured maximum size applies to each file loaded for a response.

## Code layout

The active request path is:

1. `main.c` loads configuration and initializes logging.
2. `server.c` starts the listener and accepts connections.
3. `network/socket.c` owns TCP listen and accept operations.
4. `server/connection.c` handles one client connection.
5. `http/request.c` reads and logs the request; `http/parser.c` parses its request line.
6. `static/static.c` resolves files under the document root, and
   `static/mime_types.c` maps extensions to media types.
7. `http/response.c` writes the selected file and status to the client.
8. `utils/resource_monitor.c` records per-request metrics in debug mode.

Headers in `include/` define the interfaces between these modules. The HTTP
coordinator, network buffer and event loop, router, TLS, and worker-pool files
are scaffolding and are not part of the active request path yet.
`worker_threads` is retained for the future worker-pool module; the current
server handles connections serially.

---
