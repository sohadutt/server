# Build and test

## Requirements

Build requires CMake 3.30 or newer, a C compiler, CUPS development headers and
library, SQLite 3, and OpenSSL Crypto. The integration test also requires
Python 3. CMake selects the latest C standard supported by the active compiler
and enables strict warnings for Clang and GCC.

## Build

From the project root:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Run the server with its default configuration:

```sh
./build/http_server
```

Pass a config file as the first argument to use a different configuration:

```sh
./build/http_server path/to/server.conf
```

## Run tests

Run the complete unit and integration suite:

```sh
ctest --test-dir build --output-on-failure
```

The suite includes unit tests for request parsing and validation, HTTP
responses, MIME mappings, configuration, and SQLite accounts, tokens, and
credits. The black-box integration test starts the built server on a temporary
localhost port and uses an isolated SQLite database under `build/`.

Run only one category or test by name:

```sh
ctest --test-dir build -R '^unit_' --output-on-failure
ctest --test-dir build -R integration_server --output-on-failure
ctest --test-dir build -R unit_data_store --output-on-failure
```

If a test fails, CTest prints the failing output. After fixing the cause, rerun
the failed cases with:

```sh
ctest --test-dir build --rerun-failed --output-on-failure
```

## Start scripts and HTTPS

`scripts/setup_server.sh` builds and starts the server using
`config/server.conf`:

```sh
./scripts/setup_server.sh
```

Pass a config file to use another configuration:

```sh
./scripts/setup_server.sh path/to/server.conf
```

The script does not install system packages. Install the build requirements
first. Press Ctrl-C to stop the server.

The C server speaks HTTP and does not load TLS certificates itself. For local
HTTPS, install [Caddy](https://caddyserver.com/docs/install) and run:

```sh
./scripts/setup_server.sh --tls
```

This starts Caddy at `https://localhost:8443` with its [internal local
CA](https://caddyserver.com/docs/caddyfile/directives/tls) and runs the C
server on `127.0.0.1:8080`. The generated private server config and Caddyfile
are under `build/local-tls/`; Ctrl-C stops both processes. If the browser does
not trust Caddy's certificate, run `caddy trust` once with the permissions
required by your operating system.

The local Caddy setup is for development on the same machine. For a public
host, use a real DNS name, allow Caddy to obtain a public certificate, and
reverse proxy to the server on a loopback-only address. Do not expose the
plain HTTP backend directly.

`scripts/generate_cert.sh localhost 365` creates a self-signed development
certificate with a Subject Alternative Name. It writes
`certs/dev-server.crt` and `certs/dev-server.key`, preserves existing files by
default, and accepts `--force` to replace them. The server does not load these
files directly; use this helper only for a separate TLS proxy that needs
explicit certificate paths.
