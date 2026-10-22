# Native TLS

The server can terminate TLS itself. Set `tls_enabled = true` and provide a
PEM certificate chain and matching private key in `config/server.conf`. The
certificate file should contain the server certificate first, followed by any
intermediate certificates. Keep the private key readable only by the service
account. Relative paths are resolved from the server's working directory.

When enabled, the listener accepts TLS connections only. It does not fall back
to plain HTTP on that port. The server negotiates TLS 1.2 or newer, uses
OpenSSL's maintained default cipher configuration at security level 2, disables
compression and renegotiation, disables TLS early data, and advertises only
HTTP/1.1 through ALPN. TLS 1.3 is available when supported by the installed
OpenSSL build and peer. TLS session cache entries expire after five minutes.
HTTPS responses include HSTS; plain HTTP responses do not.

The request parser, JSON endpoints, and streamed print uploads all use the same
transport abstraction. The account, admin, and print APIs reject requests that
arrive without TLS, including when the server is configured for plain HTTP.
TLS handshakes happen in the bounded worker pool and inherit the connection
receive timeout. Responses also set a restrictive Content Security Policy,
`nosniff`, anti-framing, same-origin resource, no-referrer, and no-store
headers. HSTS is sent only over HTTPS.

Generate a local development certificate with:

```sh
./scripts/setup.sh cert localhost 365
```

The helper creates an ECDSA P-256 key and a self-signed certificate with a
Subject Alternative Name. Browsers do not trust it by default. Use a
certificate issued by a trusted CA for production. See the
[build and testing guide](build-and-testing.md) for startup instructions.
