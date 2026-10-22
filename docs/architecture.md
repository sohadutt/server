# Architecture and code layout

The active request path is:

1. `main.c` loads configuration and initializes logging.
2. `server.c` starts the listener and bounded worker pool. `network/event_loop.c`
   waits for listener readiness and dispatches accept callbacks.
3. `network/socket.c` owns TCP listen and accept operations; accepted
   connections are submitted to the worker pool.
4. `server/connection.c` resolves the request with `router/router.c`, then
   dispatches API or static-file handling.
5. `server/api.c` handles login, session, admin account, and print routes.
6. `http/request.c` reads and logs the request; `http/parser.c` parses its
   request line.
7. `static/static.c` resolves files under the document root, and
   `static/mime_types.c` maps extensions to media types.
8. `http/response.c` writes the selected file and status through the shared
   HTTP transport.
9. `utils/resource_monitor.c` records per-request metrics in debug mode.
10. `storage/data.c` provides SQLite user, session, password, and credit-ledger
    storage.

`network/buffer.c` implements a bounded byte buffer with 512 bytes of inline
storage; HTTP response headers use it to avoid allocating for common replies.
`http/transport.c` provides plain socket and TLS reads/writes to the request,
response, API, and print upload handlers. `tls/tls.c` owns the OpenSSL server
context and performs TLS handshakes. Headers in `include/` define the module
interfaces. The event loop watches the listener; client sockets continue to use
the bounded worker pool so long print uploads do not block accepting new
connections.
