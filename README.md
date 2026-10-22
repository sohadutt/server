# Configurable Web and Print Server

A modular HTTP and printing service written in C. It serves configured static
files, supports browser login and document submission, discovers CUPS printers,
and stores accounts, sessions, and credit ledger entries in SQLite.

## Quick start

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./scripts/setup.sh
```

Requirements, test details, and setup/TLS steps are in the
[build and testing guide](docs/build-and-testing.md).

For a guided command-line setup that generates local TLS credentials and starts
the server, run `./scripts/setup.sh`. See the [setup guide](docs/setup.md) for
LAN access and CUPS options.

## Guides

- [Build, run tests, setup scripts, and HTTPS](docs/build-and-testing.md)
- [Guided command-line setup](docs/setup.md)
- [Point a domain at the server](docs/domain-setup/README.md)
- [Native TLS configuration and security behavior](docs/tls.md)
- [Site security model, protections, and limits](docs/security.md)
- [Server configuration and static file behavior](docs/configuration-and-static-files.md)
- [Deploy Vite builds and mounted static apps](docs/static-deployment.md)
- [Printer discovery and CUPS setup](docs/printing-and-cups.md)
- [Accounts, credits, administration, and print API](docs/accounts-and-print-api.md)
- [Load and memory benchmarking](docs/benchmarking.md)
- [Code layout and request flow](docs/architecture.md)

## Manuals

- [Manuals home](docs/manuals/README.md)
- [Getting started](docs/manuals/getting-started.md)
- [User printing guide](docs/manuals/printing.md)
- [Administrator guide](docs/manuals/administration.md)
- [Operations and maintenance](docs/manuals/operations.md)

Configuration is in `config/server.conf`; static site files are under
`public/`; scripts are under `scripts/`; public C interfaces are under
`include/`; and automated tests are under `tests/`.
