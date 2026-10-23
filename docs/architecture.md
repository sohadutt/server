# Architecture and code layout

The active request path is:

1. `main.c` loads configuration and initializes logging.
2. `server.c` starts the listener, bounded worker pool, and event loop.
   `network/event_loop.c` watches the listener, worker-completion pipe, and
   idle keep-alive sockets.
3. `network/socket.c` owns TCP listen and accept operations. A worker handles
   one request at a time. After a static response, reusable sockets return to
   the event loop and consume no worker until another request arrives.
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
interfaces. The worker queue remains bounded so slow clients and long print
uploads cannot create unbounded task memory. Idle keep-alive sockets are reaped
after 15 seconds. TLS handshakes and active request reads/writes still run
synchronously inside workers; making those phases nonblocking is a separate
step if profiling shows they dominate after idle-worker starvation is removed.

Every five seconds the server logs active connections, idle keep-alive sockets,
busy workers, accepted connections, completed worker tasks, and connections
rejected because the worker queue is full. It also reports process CPU as a
percentage of one core, and average worker queue delay, worker service time,
TLS handshake time, request-read time, response-write time, and explicit socket
writable waits. Process CPU can exceed 100% when multiple cores are active.
Response-write time includes time inside the write operation (including TLS
work); writable waits report explicit readiness waits.
