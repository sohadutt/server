# Configurable Web and Print Server

A modular HTTP and printing service written in C. It serves configured static
files, supports browser login and document submission, discovers CUPS printers,
and stores accounts, sessions, and credit ledger entries in SQLite.

## Quick start

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/http_server
```

Requirements, test details, and setup/TLS steps are in the
[build and testing guide](docs/build-and-testing.md).

## Guides

- [Build, run tests, setup scripts, and HTTPS](docs/build-and-testing.md)
- [Server configuration and static file behavior](docs/configuration-and-static-files.md)
- [Printer discovery and CUPS setup](docs/printing-and-cups.md)
- [Accounts, credits, administration, and print API](docs/accounts-and-print-api.md)
- [Load and memory benchmarking](docs/benchmarking.md)
- [Code layout and request flow](docs/architecture.md)

Configuration is in `config/server.conf`; static site files are under
`public/`; scripts are under `scripts/`; public C interfaces are under
`include/`; and automated tests are under `tests/`.
