# HTTP Server

A small C HTTP server with separate modules for configuration, TCP sockets,
connections, HTTP requests and responses, static files, CUPS printer discovery, logging,
and resource monitoring. Requests are limited to 4,095 bytes, including headers
and any request body.

## Build

Requires CMake 3.30 or newer, a C compiler, and CUPS development headers and
library. CMake selects the latest C
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
| `cups_server` | system default | Host name or IP, optionally followed by a port |
| `cups_encryption_required` | `false` | `true`, `false`, `1`, `0` |
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
The configured maximum size applies to each file loaded for a response.

The optional CUPS server and encryption settings are applied during process
startup before printer operations use the CUPS client library.

## Printer discovery and setup

The print service uses `cupsGetDests2` to read configured queues and
`cupsEnumDests` to discover network printers. Discovery is bounded by a caller
supplied timeout (up to five seconds), handles repeated updates and removal
notifications, and keeps discovered devices separate from configured queues.
The `uri` field retains the discovered device URI when CUPS supplies one.
These service functions are not exposed through an HTTP route.

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

## Future print-job JSON manifest

The application’s later job-sending layer can accept one manifest with jobs for
multiple users and documents. This is a suggested payload shape for planning;
the server does not currently parse or submit it:

```json
{
  "version": 1,
  "jobs": [
    {
      "user": "alex",
      "documents": [
        {
          "file_name": "reports/q3.pdf",
          "title": "Quarterly report",
          "format": "application/pdf",
          "settings": {
            "copies": 2,
            "media": "iso_a4_210x297mm",
            "media-source": "auto",
            "media-type": "stationery",
            "sides": "two-sided-long-edge",
            "print-color-mode": "color",
            "print-quality": "normal",
            "orientation-requested": 3,
            "number-up": 1,
            "printer-resolution": "600dpi",
            "page-ranges": "1-4",
            "print-scaling": "fit",
            "output-bin": "face-down",
            "finishings": [3]
          }
        }
      ]
    }
  ]
}
```

The future manifest deliberately has no destination field: the service assigns
each job based on current printer availability and active queue counts. Keep
the user and document metadata separate from per-document print settings. The
settings use IPP attribute names; values must be checked against the selected
printer because media names, resolutions, output bins, and finishing codes
vary. Query destination capabilities before submission instead of assuming
every printer supports every setting. The authenticated CUPS identity should
determine the actual job owner; the manifest’s `user` is application metadata
unless a separately authorized impersonation feature is designed. `file_name`
should be resolved relative to the configured document root by the future
sender.

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
