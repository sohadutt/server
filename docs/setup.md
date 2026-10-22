# Command-line setup

Run the one-command local setup from the repository root:

```sh
./scripts/setup.sh
```

It checks build dependencies, builds the C server, creates a localhost
development certificate when one is absent, writes a private TLS-enabled
runtime config, generates an admin API credential, and starts HTTPS on
`127.0.0.1:8443`. The base `config/server.conf` remains the editable source
configuration; generated credentials and runtime files stay under the ignored
`build/local-setup/` directory.

The admin credential is saved with mode `0600` at
`build/local-setup/admin.token`. Keep it private. You can regenerate it by
removing that file and running setup again; the generated runtime config is
rewritten each time.

Useful options:

```sh
./scripts/setup.sh --help
./scripts/setup.sh --check
./scripts/setup.sh --with-cups
./scripts/setup.sh --bind 0.0.0.0 --name printer.example.test --port 8443
./scripts/setup.sh --force-cert
```

`--check` configures and builds without generating credentials or starting
services. `--with-cups` starts and enables the host CUPS service; it can ask
for administrator privileges. It does not install operating-system packages
or add a printer queue. All setup tasks are available through the same script:

```sh
./scripts/setup.sh cups start
./scripts/setup.sh cups status
./scripts/setup.sh cups discover
./scripts/setup.sh cups add-printer
./scripts/setup.sh cert localhost 365
```

`cups add-printer` interactively discovers an IPP printer and creates a
driverless queue. To provide details without prompts, pass
`queue-name printer-uri "Description"`. `cert` creates a self-signed local
certificate; add `--force` after `cert` to replace an existing pair.

The default binding is loopback only. For access from other devices, bind to a
LAN interface and use a DNS name clients resolve to the host. The generated
self-signed certificate is for development and is not trusted automatically;
install a certificate from a trusted CA for shared or public deployments. The
account, admin, and print APIs reject plain HTTP requests. Use HTTPS for browser
login and document uploads.

For a public DNS domain, see the [domain deployment guide](domain-setup/README.md).

To start the service with a manually maintained config instead, see the
[build and testing guide](build-and-testing.md) and
[TLS guide](tls.md).
