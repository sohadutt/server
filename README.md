# HTTP Server

A C project organized for an HTTP server. The executable loads
`config/server.conf`, configures logging, and listens for HTTP connections on
the configured address and port.

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

---

# Project Structure

```text
http_server/
├── CMakeLists.txt
├── Makefile
├── README.md
├── LICENSE
├── include/          Public headers
├── src/
│   ├── http/
│   ├── network/
│   ├── router/
│   ├── server/
│   ├── static/
│   ├── tls/
│   ├── utils/
│   ├── workers/
│   └── main.c
├── tests/
│   ├── integration/
│   └── unit/
├── config/            Server and MIME configuration
├── certs/             Local TLS certificate files
├── public/            Static web content
└── scripts/           Development utilities
```

---
