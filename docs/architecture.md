# Architecture and code layout

The active request path is:

1. `main.c` loads configuration and initializes logging.
2. `server.c` starts the listener, accepts connections, and submits them to the
   bounded worker pool.
3. `network/socket.c` owns TCP listen and accept operations.
4. `server/connection.c` handles one client connection.
5. `server/api.c` handles login, session, admin account, and print routes.
6. `http/request.c` reads and logs the request; `http/parser.c` parses its
   request line.
7. `static/static.c` resolves files under the document root, and
   `static/mime_types.c` maps extensions to media types.
8. `http/response.c` writes the selected file and status to the client.
9. `utils/resource_monitor.c` records per-request metrics in debug mode.
10. `storage/data.c` provides SQLite user, session, password, and credit-ledger
    storage.

Headers in `include/` define interfaces between these modules. The HTTP
coordinator, network buffer and event loop, router, and TLS files are
scaffolding and are not part of the active request path yet.
