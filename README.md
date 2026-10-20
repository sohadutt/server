# HTTP Server

A small C HTTP server with separate modules for configuration, TCP sockets,
connections, HTTP requests and responses, static files, CUPS printer discovery, logging,
and resource monitoring. Requests are limited to 4,095 bytes, including headers
and any request body.

## Build

Requires CMake 3.30 or newer, a C compiler, CUPS development headers and
library, SQLite 3, and OpenSSL Crypto. CMake selects the latest C
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

## Setup scripts and HTTPS

`scripts/setup_server.sh` configures the build, compiles the server, and runs
it with `config/server.conf`:

```sh
./scripts/setup_server.sh
```

Pass a config file to use another configuration, for example
`./scripts/setup_server.sh path/to/server.conf`. The script does not install
system packages; install CMake 3.30+, a C compiler, and CUPS development files
first. Press Ctrl-C to stop the server.

The C server currently speaks HTTP and does not load TLS certificates itself.
For local HTTPS, install [Caddy](https://caddyserver.com/docs/install) and run
[`setup_server.sh --tls path/to/server.conf`](scripts/setup_server.sh) (omit
the config path to use the default):

```sh
./scripts/setup_server.sh --tls
```

This starts Caddy at `https://localhost:8443` with its [internal local CA](https://caddyserver.com/docs/caddyfile/directives/tls)
and runs the C server on `127.0.0.1:8080`, so the backend is not
directly exposed in TLS mode. The script writes its private generated server
config and Caddyfile under `build/local-tls/`; Ctrl-C stops both processes.
If the browser does not trust Caddy's local certificate, run `caddy trust`
once with the permissions required by your operating system. The token in
`print_api_token` is copied into that generated config with owner-only file
permissions.

The local Caddy setup is for development on the same machine. For a public
host, configure Caddy with your real DNS name and allow it to obtain a
publicly trusted certificate, then reverse proxy to the server on a
loopback-only address. Do not expose the plain HTTP backend directly.

`scripts/generate_cert.sh` creates a self-signed development certificate with
a Subject Alternative Name for a DNS name or IP address:

```sh
./scripts/generate_cert.sh localhost 365
```

It writes `certs/dev-server.crt` and `certs/dev-server.key`, preserves existing
files by default, and accepts `--force` to replace them. This helper does not
configure the server or the local Caddy flow; the server does not consume
these certificate files directly. Use it only when setting up a separate TLS
proxy that needs explicit certificate paths. Self-signed certificates are not
trusted by browsers unless their issuer is explicitly trusted.

## Server configuration

`config/server.conf` uses one `key = value` setting per line. Blank lines and
lines beginning with `#` are ignored. Missing settings use the defaults below.
`document_root` and `mime_types_file` paths are relative to the process working
directory unless they are absolute paths.

| Setting | Default | Valid values |
| --- | --- | --- |
| `host` | `0.0.0.0` | Host name or IP address |
| `port` | `8080` | `1`–`65535` |
| `worker_threads` | `4` | `1`–`64` |
| `document_root` | `public` | Existing directory |
| `index_file` | `index.html` | Relative file inside document root |
| `not_found_page` | `404.html` | Relative file inside document root |
| `mime_types_file` | `config/mime.types` | MIME mapping file |
| `default_mime_type` | `application/octet-stream` | Fallback media type |
| `max_static_file_size_mb` | `64` | `1`–`256` |
| `cups_server` | system default | Host name or IP, optionally followed by a port |
| `cups_encryption_required` | `false` | `true`, `false`, `1`, `0` |
| `print_api_token` | disabled | Private bearer token, 32–255 characters |
| `log_level` | `info` | `debug`, `info`, `warn`, `error` |
| `log_console` | `true` | `true`, `false`, `1`, `0` |
| `log_file` | `true` | `true`, `false`, `1`, `0` |
| `log_directory` | `logs` | Non-empty path |

Unknown settings and invalid values stop startup with the config filename and
line number.

With `log_level = debug`, each handled connection also logs request timing,
CPU time, page faults, context switches, bytes received and sent, and the
server process's peak resident memory. Request headers are logged with
authorization and cookie values redacted.

## Static file selection

Static files are served with `GET`. The URL path maps to a file below
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
The configured maximum size applies to each file loaded for a response. A
shared 256 MiB buffer budget bounds memory used by concurrent file responses;
workers wait for budget to be released before loading another file. Files are
opened relative to the held document-root directory descriptor, with symlinks
rejected at each path component.

Connections are handled by a fixed worker pool with a bounded queue of eight
pending connections per worker. When the queue is full, new connections are
closed. Request headers have a ten-second total receive deadline, and socket
read/write timeouts bound worker time for stalled peers.

The optional CUPS server and encryption settings are applied during process
startup before printer operations use the CUPS client library.

## Printer discovery and setup

The print service uses `cupsGetDests2` to read configured queues and
`cupsEnumDests` to discover network printers. Discovery is bounded by a caller
supplied timeout (up to five seconds), handles repeated updates and removal
notifications, and keeps discovered devices separate from configured queues.
The `uri` field retains the discovered device URI when CUPS supplies one.
The browser print page is available at `/print/`. Normal URLs continue to serve
the configured static HTML, CSS, JavaScript, and other files.

Before assigning a future print job, `select_printer()` refreshes the active
job count for each configured queue across all users. It skips queues that
reject jobs or are stopped, prefers an idle queue with no active jobs, and
otherwise chooses the eligible queue with the fewest active jobs. Ties prefer
an idle queue, then the CUPS default queue, then the alphabetically first name.
It returns `NULL` if no eligible queue is available or CUPS cannot refresh the
counts. Assignment belongs to the service; clients should not choose a printer
in the job manifest.

Run `scripts/add_printer.sh` to list discovered IPP destinations, choose one,
and create a driverless CUPS queue. The script uses [`lpadmin`](https://openprinting.github.io/cups/doc/man-lpadmin.html); adding a queue
requires CUPS administrator privileges. `cupsAddDest` only creates a saved
options entry for a queue that already exists, so it is not used to add a
printer queue. The [CUPS Programming Manual](https://openprinting.github.io/cups/doc/cupspm.html)
describes the destination enumeration callbacks and their update/removal flags.

To start and inspect the local CUPS scheduler, use:

```sh
./scripts/setup_cups.sh
./scripts/setup_cups.sh --discover
./scripts/setup_cups.sh --add-printer
```

The setup helper supports systemd on Linux and launchd on macOS, and asks for
administrator privileges when the service manager needs them. It does not
install packages or enable remote CUPS administration. `--status` checks the
scheduler and configured queues. `--discover` lists IPP destinations, and
`--add-printer` runs the existing interactive driverless queue setup. Once a
queue exists, the server's `cups_server` and
`cups_encryption_required` settings control how this application connects to
the scheduler; they do not configure printer hardware or CUPS itself.

## User, token, and credit storage

`include/data.h` and `src/storage/data.c` provide a SQLite storage module. It
creates the schema on first open and stores E.164 phone IDs, active status,
credit balances, token digests, and an append-only credit ledger. Build now
links SQLite and OpenSSL Crypto for persistence and secure token generation.

The module returns a newly generated bearer token only when creating or issuing
it. The database stores only its SHA-256 digest. Keep the returned token
private; it cannot be recovered from the database. Deactivating a user blocks
their tokens while retaining their account and ledger history. Revoke or
replace credentials rather than storing raw token strings.

Balances use signed 64-bit integer credit units, never floating-point money.
`data_credit_add()` should only be called after a future payment flow has
verified a payment, and `data_credit_charge()` atomically checks and debits a
balance. A non-null reference is unique per user, so retries of a payment or
print charge can be detected instead of applying the same ledger event twice.
The module does not process payments, choose a price per page, or connect user
accounts and credits to `/api/print` yet. Those rules and authorization flow
can be added when you define how payments and print charges should behave.

The caller opens a private database path and closes the store after all worker
operations have finished. For example, pass `data/printing.sqlite` after
creating the `data` directory with owner-only permissions. SQLite uses WAL,
foreign keys, a busy timeout, full synchronous writes, secure deletion, and
0600 permissions on the database file.

```sh
mkdir -m 700 data
```

`data_user_create()` creates the phone-number account and returns its initial
token once. `data_user_list()` enumerates accounts, `data_token_authenticate()`
resolves an active token to its user and current balance, and token revocation
can revoke one token or all of a user's tokens. A future payment handler can
call `data_credit_add()` after verifying a provider transaction; print submission can call
`data_credit_charge()` with a stable job reference before accepting a job.
Those application flows are intentionally not wired into the HTTP route yet.

## Browser printing and upload API

Set `print_api_token` in `config/server.conf` to a private random value of at
least 32 characters to enable print submissions. For example, generate one
with `openssl rand -hex 32`; do not commit or share the value. The print page
asks for the token and keeps it in the current browser tab's session storage.
Without a configured token, `/api/print` returns `503` and does not accept jobs.

`POST /api/print` takes the document as the raw request body (not base64 or a
JSON manifest). It accepts PDF, JPEG, and PNG uploads up to 64 MiB. The request
must include `Authorization: Bearer <token>` and `Content-Type` set to the
document's media type. Optional settings are sent as headers:

| Header | Values | Default |
| --- | --- | --- |
| `X-Print-Copies` | `1`–`99` | `1` |
| `X-Print-Sides` | `one-sided`, `two-sided-long-edge`, `two-sided-short-edge` | `one-sided` |
| `X-Print-Pages` | PDF only, for example `1-3,5` | All pages |

The endpoint spools one request at a time to a temporary file, selects an
eligible configured CUPS queue using the service's active-job counts, then
streams the document to CUPS. The response is JSON containing the assigned
printer and CUPS job ID. Example:

```sh
curl --fail-with-body \
  -H 'Authorization: Bearer YOUR_PRIVATE_TOKEN' \
  -H 'Content-Type: application/pdf' \
  -H 'X-Print-Copies: 2' \
  -H 'X-Print-Sides: two-sided-long-edge' \
  -H 'X-Print-Pages: 1-3,5' \
  --data-binary @document.pdf \
  http://localhost:8080/api/print
```

The browser page exposes these settings in a compact, mobile-friendly form.
Printer selection stays server-side so a client cannot force a job onto a
specific queue. The current endpoint accepts one document per request; a
multi-user JSON job manifest is not part of this upload contract.

## Code layout

The active request path is:

1. `main.c` loads configuration and initializes logging.
2. `server.c` starts the listener, accepts connections, and submits them to the
   bounded worker pool.
3. `network/socket.c` owns TCP listen and accept operations.
4. `server/connection.c` handles one client connection.
5. `http/request.c` reads and logs the request; `http/parser.c` parses its request line.
6. `static/static.c` resolves files under the document root, and
   `static/mime_types.c` maps extensions to media types.
7. `http/response.c` writes the selected file and status to the client.
8. `utils/resource_monitor.c` records per-request metrics in debug mode.
9. `storage/data.c` provides the SQLite user, token, and credit-ledger module
   for future account and billing integration.

Headers in `include/` define the interfaces between these modules. The HTTP
coordinator, network buffer and event loop, router, and TLS files are scaffolding
and are not part of the active request path yet.

---
