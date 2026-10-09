# HTTP Server

A small C HTTP server with separate modules for configuration, TCP sockets,
connections, HTTP requests and responses, static files, logging, and resource
monitoring. The server reads up to 4,095 bytes from a client and serves files
from the configured document root.

## Build

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
lines beginning with `#` are ignored. Missing settings use these defaults:

| Setting | Default | Valid values |
| --- | --- | --- |
| `host` | `0.0.0.0` | Host name or IP address |
| `port` | `8080` | `1`–`65535` |
| `worker_threads` | `4` | `1`–`1024` |
| `document_root` | `public` | Non-empty path |
| `log_level` | `info` | `debug`, `info`, `warn`, `error` |
| `log_console` | `true` | `true`, `false`, `1`, `0` |
| `log_file` | `true` | `true`, `false`, `1`, `0` |
| `log_directory` | `logs` | Non-empty path |

Unknown settings and invalid values stop startup with the config filename and
line number.

With `log_level = debug`, each handled connection also logs request timing,
CPU time, page faults, context switches, bytes received and sent, and the
server process's peak resident memory.

## Code layout

The active request path is:

1. `main.c` loads configuration and initializes logging.
2. `server.c` starts the listener and accepts connections.
3. `network/socket.c` owns TCP listen and accept operations.
4. `server/connection.c` handles one client connection.
5. `http/request.c` reads and logs the request; `http/parser.c` parses its request line.
6. `static/static.c` safely resolves and loads files under the document root.
7. `http/response.c` writes the selected file and status to the client.
8. `utils/resource_monitor.c` records per-request metrics in debug mode.

Headers in `include/` define the interfaces between these modules. The event
loop, router, static file, TLS, and worker-pool files are scaffolding and are
not part of the active request path yet. `worker_threads` and `document_root`
are loaded from configuration for those future modules; the current handler
uses a fixed response and one synchronous connection at a time.

---
