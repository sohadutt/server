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
./scripts/setup.sh static build dashboard /path/to/dashboard https://print.example.com
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

The print page stamps an identity QR onto the first page of the first document
in a grouped submission using locally hosted JavaScript libraries. QR creation,
PDF page selection, and image conversion happen in the browser; the server
does not need PDF-processing system tools. See the
[browser printing manual](manuals/printing.md) for supported behavior.

For a public DNS domain, see the [domain deployment guide](domain-setup/README.md).
To publish a separate Vite app on the same domain, use the
[static deployment guide](static-deployment.md). It builds the app with the
right URL base, installs it below `public/`, and prints the link.

## SQLite backups

Create a consistent database snapshot while the service is running:

```sh
python3 scripts/backup_db.py
```

By default, this snapshots `data/server.sqlite` into a private `backups/`
directory. Pass the database path and backup directory as positional arguments
when using a custom config. The script uses SQLite's online backup API, checks
the snapshot integrity, writes it atomically with mode `0600`, and does not
stop or lock out active server requests. Protect backups like the live database
because they contain phone numbers, password hashes, sessions, and print
history. Copy backups off-host and schedule retention according to deployment
policy.

To start the service with a manually maintained config instead, see the
[build and testing guide](build-and-testing.md) and
[TLS guide](tls.md).
