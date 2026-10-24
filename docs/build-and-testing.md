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

The unified setup script can build and start the server through local Caddy.
The browser-facing endpoint negotiates HTTP/2; Caddy can also use HTTP/3 when
UDP is available. The C backend stays on loopback:

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

The guided local HTTPS flow and `manage.sh server` install/use Caddy and serve
the browser-facing site at `https://localhost:8443`. Caddy proxies to the
native server over loopback TLS on port 8444. Use
`manage.sh server --native-tls` only when HTTP/1.1 from the C server itself is
desired.

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

To run the C server directly with native TLS (HTTP/1.1), use:

```sh
./scripts/manage.sh server --native-tls
```

The C server's direct TLS listener advertises HTTP/1.1 only. For HTTP/2 locally,
use the default `manage.sh server` or the guided menu. Caddy uses its [internal
local CA](https://caddyserver.com/docs/caddyfile/directives/tls); run `caddy
trust` if the browser does not trust it. The generated backend config and
Caddyfile are under `build/local-setup/` (or `build/local-tls/` for the explicit
`--tls` mode). Ctrl-C stops both processes. For public HTTP/2/HTTP/3 use the
[domain guide](domain-setup/README.md).

`scripts/manage.sh cert` creates a P-256 self-signed development certificate
with a Subject Alternative Name. Existing files are preserved by default;
pass `--force` to replace them.
