# Point a domain at the server

This guide connects a domain such as `print.example.com` to this server and
serves the site over public HTTPS. It uses Caddy as the public TLS endpoint and
keeps the C application bound to loopback. The application requires TLS for its
account, admin, and print endpoints, so Caddy connects to the local server over
HTTPS too.

Replace `print.example.com` with a domain you own and `/path/to/server` with
the absolute path to this repository.

## 1. Point DNS to the server

Find the server's public IP address in your hosting provider's dashboard. At
your DNS provider, create records like these:

| Type | Name | Value |
| --- | --- | --- |
| A | `print` | The server's public IPv4 address |
| AAAA | `print` | The server's public IPv6 address, only if IPv6 is configured and reachable |

For the root domain (`example.com`), use `@` as the name if your DNS provider
uses that convention. Do not publish an AAAA record unless the server really
accepts IPv6 traffic; a stale IPv6 address can make browsers fail even when the
IPv4 record is correct. If your public IP changes, configure a static IP or a
DNS provider's dynamic DNS update.

Allow inbound TCP ports 80 and 443 through the host firewall and any cloud
firewall/security group. If the server is behind a home router, forward those
ports to it. Do not expose the application's ports 8443 or 8444 publicly.
Allow inbound UDP port 443 as well if clients should be able to use HTTP/3;
HTTP/2 continues to use TCP port 443.

Check that DNS resolves before asking Caddy to obtain a certificate:

```sh
dig +short A print.example.com
dig +short AAAA print.example.com
```

The returned addresses should be the public addresses that reach this host.
DNS changes can take time to propagate.

## 2. Create the local backend certificate and start the app

From the repository root, create the backend certificate for the same hostname
and start the app on loopback:

```sh
./scripts/manage.sh cert print.example.com 365
./scripts/manage.sh --bind 127.0.0.1 --name print.example.com --port 8443
```

The certificate is self-signed and is used only between Caddy and the local C
server. It is not the certificate visitors see. The setup command builds the
server, creates a private runtime config and admin token under
`build/local-setup/`, then runs the server in the foreground. Keep it running
while configuring Caddy in another terminal. For long-term use, run the server
under your operating system's service manager using the generated config at
`build/local-setup/server.conf`; run it as the account that owns the generated
private key and runtime files. For example, a Linux systemd service can use:

```ini
[Unit]
Description=Web and print server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=YOUR_SERVICE_USER
WorkingDirectory=/path/to/server
ExecStart=/path/to/server/build/http_server /path/to/server/build/local-setup/server.conf
LimitNOFILE=65536
Restart=on-failure
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

Replace the user and both paths, save this as
`/etc/systemd/system/print-server.service`, then run
`sudo systemctl daemon-reload` and `sudo systemctl enable --now print-server`.
Make sure the service user can write the configured database and log paths.

If `certs/server.crt` and `certs/server.key` already exist for another
hostname, the `cert` command will preserve them and stop. To rotate them, run
`./scripts/manage.sh cert --force print.example.com 365` while the server is
stopped, then start the server again. The private key is replaced as part of
that operation.

## 3. Configure Caddy for the public domain

Install Caddy using its [official installation instructions](https://caddyserver.com/docs/install).
Create a Caddyfile with this site block, replacing the domain and repository
path. On Linux, copy the public backend certificate into Caddy's readable
configuration directory so its service can verify it:

```sh
sudo install -m 0644 certs/server.crt /etc/caddy/server-backend.crt
```

```caddyfile
print.example.com {
    reverse_proxy https://127.0.0.1:8444 {
        transport http {
            tls_trust_pool file /etc/caddy/server-backend.crt
            tls_server_name print.example.com
        }
    }
}
```

This makes Caddy verify the backend certificate against the certificate file
and hostname. Keep that trust check enabled. The trust-certificate path must
be absolute and readable by the Caddy service account. After rotating the
backend certificate, copy the new public certificate to
`/etc/caddy/server-backend.crt` and reload Caddy so it reads the new trust
certificate.

For a first run, start Caddy in the foreground with the Caddyfile path:

```sh
caddy run --config /path/to/Caddyfile
```

Once it works, install/enable Caddy as a system service using the instructions
for your OS and put the site block in the service's Caddyfile (commonly
`/etc/caddy/Caddyfile` on Linux). Keep the C server running as a service too.
Caddy automatically obtains and renews a public certificate when the domain
resolves to this host and its validation ports are reachable. See Caddy's
[HTTPS setup](https://caddyserver.com/docs/quick-starts/https) and
[reverse proxy documentation](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy).
Caddy enables HTTP/2 and HTTP/3 on HTTPS sites by default. HTTP/3 requires
clients and the network path to support QUIC over UDP; otherwise clients use
HTTP/2 or HTTP/1.1 automatically.

## 4. Verify the public site

Visit `https://print.example.com/` and check the print page at
`https://print.example.com/print/`. HTTP should redirect to HTTPS. From a
terminal, check the response:

```sh
curl -I https://print.example.com/
curl -I http://print.example.com/
```

If Caddy cannot issue a certificate, check DNS resolution, TCP 80/443 access,
router forwarding, cloud firewall rules, and whether another service already
owns those ports. Caddy's [troubleshooting guide](https://caddyserver.com/docs/troubleshooting)
lists common causes.

Mounted Vite and static applications use paths on the same domain, for example
`https://print.example.com/dashboard/`. The Caddy site block above already
proxies all paths to the C server; add the application mount with
[`manage.sh static`](../static-deployment.md), then restart the C server.

## Important deployment notes

- Keep ports 8443 and 8444 loopback-only. Caddy should be the only public web listener.
- Keep `build/local-setup/admin.token`, the generated server config, the
  database, and TLS private key readable only by the service account/admins.
- Use a stable public IP or keep DNS updated when the public address changes.
- A domain name only points traffic to the server; it does not provide hosting,
  a public IP, firewall rules, or an email service.
- The generated `manage.sh` runtime config is for this deployment flow. Review
  [site security](../security.md) and [TLS behavior](../tls.md) before exposing
  the service to the internet.
