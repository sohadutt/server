# Command-line setup

Run the interactive menu from the repository root:

```sh
./scripts/manage.sh
```

The menu offers guided HTTPS setup, config generation, build checks, CUPS
printer setup, static app publishing, and development certificates. Use
`./scripts/manage.sh menu` to reopen it explicitly. All actions also have
non-interactive command forms.

If `config/server.conf` is missing or empty, setup generates a complete private
config there and asks whether it is for local development or a public server.
The guided setup creates a private HTTPS runtime config, generates an admin API
credential, and starts Caddy at `127.0.0.1:8443`; Caddy negotiates HTTP/2 and
proxies to the native TLS server on loopback port 8444. The public profile is
for a directly exposed native TLS listener and requires a trusted certificate
and matching key when started with `--native-tls`. Generated credentials and
runtime files stay under the ignored `build/local-setup/` directory.

The admin credential is saved with mode `0600` at
`build/local-setup/admin.token`. Keep it private. You can regenerate it by
removing that file and running setup again; the generated runtime config is
rewritten each time.

Useful options:

```sh
./scripts/manage.sh --help
./scripts/manage.sh --check
./scripts/manage.sh --start
./scripts/manage.sh --with-cups
./scripts/manage.sh --bind 0.0.0.0 --name printer.example.test --port 8443
./scripts/manage.sh --force-cert
./scripts/manage.sh config init
./scripts/manage.sh config init --profile public
```

Whenever `manage.sh` starts the server, it raises the soft open-file limit to
65,536 when the operating system permits and reports the effective and hard
limits. Set `SERVER_NOFILE_LIMIT` to request a different value. If the hard
limit is too low, raise it in the host or service manager. Generated systemd
units request `LimitNOFILE=65536`. The starter config selects an automatic
worker count based on online CPUs (at least 4, at most 64); set
`worker_threads = N` in the config to choose a fixed count.

`--check` configures and builds without generating credentials or starting
services. `--start` launches an existing binary through Caddy without
rebuilding; it prefers the guided runtime config when available and otherwise
generates `config/server.conf`. `config init` uses the tracked
`config/server.conf.example` template, writes mode `0600`, and refuses to
replace a non-empty file unless `--force` is supplied. The interactive profile
choice defaults to local; non-interactive commands also default to local unless
`--profile public` is passed. `--config FILE` selects the base config used when
the guided flow writes its generated runtime config. Build and Vite dependencies
are checked and missing packages are installed using Homebrew on macOS or apt,
dnf, pacman, or zypper on supported Linux distributions. Package installation
may require administrator privileges. CUPS client tools are installed on
supported Linux distributions when a CUPS command needs them. `--with-cups`
starts and enables the host CUPS service. All setup tasks are available through
the same script:

```sh
./scripts/manage.sh cups start
./scripts/manage.sh cups status
./scripts/manage.sh cups discover
./scripts/manage.sh cups add-printer
./scripts/manage.sh cert localhost 365
./scripts/manage.sh static build dashboard /path/to/dashboard https://print.example.com
```

`cups add-printer` interactively discovers an IPP printer and creates a
driverless queue. To provide details without prompts, pass
`queue-name printer-uri "Description"`. `cert` creates a self-signed local
certificate; add `--force` after `cert` to replace an existing pair.
For public profile startup, install a trusted certificate and key at the paths
in `config/server.conf` (defaults: `certs/server.crt` and `certs/server.key`).

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
