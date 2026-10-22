# Configuration and static files

## Configuration

`config/server.conf` uses one `key = value` setting per line. Blank lines and
lines beginning with `#` are ignored. Missing settings use these defaults.
`document_root`, `mime_types_file`, and TLS certificate/key paths are relative
to the process working directory unless they are absolute paths.

| Setting | Default | Valid values |
| --- | --- | --- |
| `host` | `0.0.0.0` | Host name or IP address |
| `port` | `8080` | `1`–`65535` |
| `worker_threads` | `4` | `1`–`64` |
| `tls_enabled` | `false` | `true`, `false`, `1`, `0` |
| `tls_certificate_file` | unset | PEM certificate chain; required when TLS is enabled |
| `tls_private_key_file` | unset | PEM private key matching the certificate; required when TLS is enabled |
| `document_root` | `public` | Existing directory |
| `index_file` | `index.html` | Relative file inside document root |
| `not_found_page` | `404.html` | Relative file inside document root |
| `mime_types_file` | `config/mime.types` | MIME mapping file |
| `default_mime_type` | `application/octet-stream` | Fallback media type |
| `max_static_file_size_mb` | `64` | `1`–`256` |
| `signup_enabled` | `false` | `true`, `false`, `1`, `0`; enable only when phone ownership is verified |
| `static_mount` | none | Repeatable: `/path|directory|spa` or `/path|directory|files` |
| `cups_server` | system default | Host name or IP, optionally followed by a port |
| `cups_encryption_required` | `false` | `true`, `false`, `1`, `0` |
| `database_path` | `data/printing.sqlite` | SQLite database file path |
| `admin_api_token` | disabled | Admin bearer secret, 32–255 characters |
| `log_level` | `info` | `debug`, `info`, `warn`, `error` |
| `log_console` | `true` | `true`, `false`, `1`, `0` |
| `log_file` | `true` | `true`, `false`, `1`, `0` |
| `log_directory` | `logs` | Non-empty path |
| `log_retention_days` | `30` | `1`–`3650` |

Unknown settings and invalid values stop startup with the config filename and
line number. With `log_level = debug`, each handled connection also logs
request timing, CPU time, page faults, context switches, bytes received and
sent, and the server process's peak resident memory. Authorization and cookie
values are redacted from logged request headers.

Production uses `log_level = info` by default. Log files use the local date in
their name, such as `2026-10-22.log`. At startup the logger removes only
regular files matching `YYYY-MM-DD.log` that are older than
`log_retention_days`; it leaves other files and symbolic links untouched.

## Static file mapping

Static files are served with `GET`. URL paths map below `document_root`:

| URL | File |
| --- | --- |
| `/` | configured `index_file` |
| `/about.html` | `document_root/about.html` |
| `/assets/app.js` | `document_root/assets/app.js` |

A directory URL selects the configured index file within that directory.
Query strings do not affect file selection. Paths that try to leave the
document root are rejected. Missing files return the configured
`not_found_page` with a `404` status.

`mime_types_file` uses one `extension media/type` pair per line, with blank
lines and `#` comments allowed. For example, `js text/javascript` makes `.js`
files use `Content-Type: text/javascript`. Extensions are matched without
case sensitivity. Unmapped extensions use `default_mime_type`.

The configured maximum size applies to each response file. A shared 256 MiB
buffer budget bounds memory used by concurrent file responses; workers wait
for budget to be released before loading another file. Files are opened
relative to the held document-root directory descriptor, with symlinks
rejected at each path component.

## Mounted static applications

Mounts publish built frontends below URL prefixes. Vite single-page
applications use `spa` mode, which returns the app's `index.html` for unknown
paths under that prefix so client-side routes work. `files` mode serves files
and directory indexes without SPA fallback. The directory is relative to
`document_root`; normal traversal and symlink protections still apply.

```ini
static_mount = /portal|apps/portal|spa
static_mount = /manual|apps/manual|files
```

Build a Vite app with base `/portal/`. The setup CLI sets this automatically,
installs the output under `public/apps/portal`, updates this config, and prints
the public URL. New mounts take effect after restarting the server. See the
[static deployment guide](static-deployment.md).

## Connection limits

Connections use a fixed worker pool and a bounded queue of eight pending
connections per worker. When the queue is full, new connections are closed.
Request headers have a ten-second total receive deadline, and socket
read/write timeouts bound worker time for stalled peers.

The optional CUPS server and encryption settings are applied during process
startup before printer operations use the CUPS client library.
