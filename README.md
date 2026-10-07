# HTTP Server

A C project organized for an HTTP server. The current executable loads
`config/server.conf`, initializes the file logger, writes sample log messages,
and exits. The HTTP, networking, routing, TLS, and worker source files are
currently placeholders.

## Build

```sh
cmake -S . -B build
cmake --build build
```

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
