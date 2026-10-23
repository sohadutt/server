# Build and test

## Requirements

Build requires CMake 3.30 or newer, a C compiler, CUPS development headers and
library, SQLite 3, and OpenSSL 3.0 or newer with SSL and Crypto. The integration
test also requires Python 3. CMake selects the latest C standard supported by the active compiler
and enables strict warnings for Clang and GCC.

## Build

From the project root:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Use the guided CLI setup to create a development certificate and start the
TLS-enabled server:

```sh
./scripts/manage.sh
```

For a manually configured server, pass a config file to the binary:

```sh
./build/http_server path/to/server.conf
```

## Run tests

Run the complete unit and integration suite:

```sh
ctest --test-dir build --output-on-failure
```

The suite includes focused unit tests for request parsing and validation,
bounded buffers, HTTP responses, route matching, MIME mappings, configuration,
the listener event loop, bounded thread-pool behavior, static-file loading and
path safety, bounded static streaming and cache policy, and SQLite accounts,
tokens, and credits. The black-box
integration test starts the built server on a temporary localhost port and
uses an isolated SQLite database under `build/`.

CI builds with strict compiler warnings, runs the suite in release mode, and
runs it again with AddressSanitizer and UndefinedBehaviorSanitizer. A separate
Clang/libFuzzer job exercises the HTTP request-line parser. To run that target
locally with Clang:

```sh
CC=clang cmake -S . -B build-fuzz -DENABLE_FUZZING=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz --target fuzz_http_parser --parallel
./build-fuzz/fuzz_http_parser -max_total_time=60
```

Run only one category or test by name:

```sh
ctest --test-dir build -R '^unit_' --output-on-failure
ctest --test-dir build -R integration_server --output-on-failure
ctest --test-dir build -R unit_data_store --output-on-failure
ctest --test-dir build -R unit_static_files --output-on-failure
ctest --test-dir build -R unit_thread_pool --output-on-failure
ctest --test-dir build -R unit_router --output-on-failure
ctest --test-dir build -R unit_mime_types --output-on-failure
```

If a test fails, CTest prints the failing output. After fixing the cause, rerun
the failed cases with:

```sh
ctest --test-dir build --rerun-failed --output-on-failure
```

## Start scripts and HTTPS

The unified setup script can build and start the server using
`config/server.conf`:

```sh
./scripts/manage.sh server
```

Pass a config file to use another configuration:

```sh
./scripts/manage.sh server path/to/server.conf
```

On Linux, create a systemd user-service unit with:

```sh
./scripts/manage.sh service-unit path/to/server.conf
```

Review the generated writable paths before enabling the unit; see the
[operations guide](manuals/operations.md).

When invoked through `manage.sh`, missing build packages are installed using
Homebrew on macOS or apt, dnf, pacman, or zypper on supported Linux
distributions. This may request administrator privileges. Direct CMake builds
still require those tools and libraries to be installed already. If the server
config is missing, `manage.sh server` generates `config/server.conf` and asks
whether to use the local or public profile. Press Ctrl-C to stop the server.

Native HTTPS is built into the C server. Generate a self-signed certificate for
local development:

```sh
./scripts/manage.sh cert localhost 365
```

Set these values in `config/server.conf` (certificate paths are relative to the
working directory):

```ini
tls_enabled = true
tls_certificate_file = certs/server.crt
tls_private_key_file = certs/server.key
port = 8443
```

Then start the native TLS listener:

```sh
./scripts/manage.sh server --native-tls
```

When TLS is enabled, the listener accepts HTTPS only. Browsers will warn about
the development certificate because it is self-signed. Public deployments
should use a CA-issued certificate and keep the private key readable only by
the service account.

For local HTTPS through an optional reverse proxy, install
[Caddy](https://caddyserver.com/docs/install) and run:

```sh
./scripts/manage.sh server --tls
```

This starts Caddy at `https://localhost:8443` with its [internal local
CA](https://caddyserver.com/docs/caddyfile/directives/tls) and runs the C
server with TLS on `127.0.0.1:8080`. The generated private server config and Caddyfile
are under `build/local-tls/`; Ctrl-C stops both processes. If the browser does
not trust Caddy's certificate, run `caddy trust` once with the permissions
required by your operating system.

The local Caddy setup is for development on the same machine. For a public
host, use a real DNS name, allow Caddy to obtain a public certificate, and
reverse proxy to the server on a loopback-only address. Do not expose the
plain HTTP backend directly.

`scripts/manage.sh cert` creates a P-256 self-signed development certificate
with a Subject Alternative Name. Existing files are preserved by default;
pass `--force` to replace them.
