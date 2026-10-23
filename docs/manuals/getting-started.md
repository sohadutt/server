# Getting started

## Requirements

The build needs CMake 3.30 or newer, a C23-capable compiler, CUPS development
headers and library, SQLite 3, and OpenSSL 3.0 or newer. The guided setup also
uses OpenSSL command-line tools to create a local development certificate.

Install those requirements using your operating system's package manager, then
run these commands from the repository root:

```sh
./scripts/manage.sh --check
./scripts/manage.sh
```

The first command configures and builds the project.
The second creates a local certificate if needed, creates an admin credential,
and starts the server at `https://localhost:8443/`. The generated private
configuration and admin token are stored in `build/local-setup/`.

The certificate is self-signed, so your browser will warn that it is not
trusted. This is suitable for local development. Do not use it as the public
certificate for an Internet-facing site.

## Open the site

- Main static site: `https://localhost:8443/`
- Sign-in and printing page: `https://localhost:8443/print/`

The server serves HTML, CSS, JavaScript, and other static files from `public/`.
The print page sends account and document requests to the same server over
HTTPS.

To stop the foreground server, press **Ctrl-C** in the terminal running
`manage.sh`.

## Add a printer

The server submits print jobs through CUPS. Start and inspect CUPS, then add a
queue if necessary:

```sh
./scripts/manage.sh cups start
./scripts/manage.sh cups status
./scripts/manage.sh cups discover
./scripts/manage.sh cups add-printer
```

Printer discovery may show devices that are not yet configured as queues. The
`add-printer` command creates a driverless queue for a discovered IPP printer.
After adding one, run `cups status` again to check that CUPS reports it as
ready and accepting jobs.

## Configure the server

Edit `config/server.conf` for port, host, TLS certificate and key, log
settings, static-file paths, CUPS connection, and database location. Relative
file paths are resolved from the server's working directory. Use a private
runtime configuration for secrets such as `admin_api_token`; do not commit
secrets to source control.

The default CUPS configuration uses the host's local scheduler. See the
[CUPS and printer guide](../printing-and-cups.md) for queue setup and
assignment behavior. See the [configuration reference](../configuration-and-static-files.md)
for every server setting.

## Allow access from other devices

For a trusted local network, bind the server to the machine's LAN interface
and use a certificate that clients trust. For a public site, configure DNS,
firewall rules, and a public TLS proxy. Follow the [domain deployment guide](../domain-setup/README.md)
and review the [security guide](../security.md) before exposing it to the
Internet.

## Browser print preparation

The print page creates the identity QR, selects PDF pages, and converts images
in the browser before upload. PDF and QR libraries are served from
`public/print/vendor/`; no server PDF tools or third-party CDN are required.
Users can turn off QR stamping under **Advanced print settings**.
