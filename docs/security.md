# Site and connection security

This document describes protections implemented in this repository and their
limits. They reduce common risks; they cannot guarantee that credentials are
impossible to copy, that every document is safe, or that a caller is a real
person just because they know a phone number.

## Connection protection

When `tls_enabled = true`, the listener accepts TLS connections only. The
server negotiates TLS 1.2 or newer, disables TLS compression, renegotiation,
and early data, and advertises HTTP/1.1. Certificate verification is performed
by the client: use a certificate issued for the hostname clients visit and
trusted by those clients. The self-signed certificate made by the development
scripts is not trusted automatically.

The account, admin, and print API routes also check that the connection is
actually TLS-protected. If the listener is configured for plain HTTP, these
routes respond with `426 Upgrade Required`; they do not accept passwords,
bearer tokens, or print uploads over that connection. The static site itself
can still be served over HTTP when TLS is disabled, so use HTTPS for the whole
site whenever it is reachable from another device or network.

TLS protects data in transit from passive network observers and helps prevent
network modification when the client validates the server certificate. It
does not protect data on an infected device, validate an untrusted
self-signed certificate, or encrypt the SQLite database at rest.

## Login and identity

Passwords are 8–128 bytes and are stored as PBKDF2-HMAC-SHA256 hashes, with a
random 16-byte salt and 310,000 iterations. The server compares password
hashes with a constant-time comparison and clears temporary password-derived
buffers after use. Password changes revoke the account's current sessions.

After signup or login, the server creates a cryptographically random 32-byte
bearer token. SQLite stores only its SHA-256 digest. Tokens expire after 30
days, are checked against expiry and revocation on each authenticated request,
and are rejected for inactive accounts. Logout revokes the current token;
password reset and account deactivation revoke existing tokens. The browser
keeps its token in memory rather than browser storage, so refreshing the page
requires another login.

Each bearer token is rate-limited independently using its SHA-256 digest: 120
requests for a verified active session and 3 print submissions per minute.
Invalid or revoked tokens do not allocate session-limit entries; they remain
covered by the per-client API limit. Limits are in-process and reset on
restart. Admins can revoke one session through
`POST /api/admin/users/token/revoke`; the database retains only the token hash
and revocation timestamp, and authentication rejects it immediately.

Bearer tokens are **copyable credentials**. Anyone who obtains a valid token
can replay it as that user until it expires or is revoked. TLS, memory-only
browser storage, redacted authorization logging, and token revocation reduce
exposure, but this application does not bind tokens to a device or use
sender-constrained tokens. Malware, a malicious browser extension, or an
injection flaw that executes script in the site can still access a token while
the page is running.

Phone numbers are account identifiers, not verified identity. Public signup is
disabled by default with `signup_enabled = false`; administrators can create
accounts. Enabling signup does not send an SMS or prove control of the number,
so limit it to trusted development until an SMS verification provider is
configured. The service does not have multi-factor authentication.
Login, signup, and admin routes have in-process per-client limits of 12, 5,
and 30 requests per 60 seconds, and remaining API traffic is capped at 300 per
client per minute. Malformed HTTP requests get at most 10 error responses per
client per minute, after which the server silently closes the connection.
These limits are memory-only, reset on restart,
and are not shared between server instances; public multi-instance deployments
should also enforce limits at a trusted edge.

Printed QR codes contain only the internal numeric user ID, not the phone
number. They are unsigned lookup hints and must never be accepted as proof of
identity or as an authentication credential.

The admin API uses a configured bearer secret of at least 32 characters and a
constant-time comparison. Keep it private. The CLI account manager accesses
SQLite directly, so operating-system permissions on the database and host
account control access to that tool.

## HTTP parsing and injection defenses

The server accepts HTTP/1.0 and HTTP/1.1 request lines with origin-form paths.
It bounds request headers, applies connection and header-read timeouts, and
rejects invalid request-line/header syntax, repeated `Host` or
`Content-Length`, ambiguous body lengths, and `Transfer-Encoding` rather than
trying to support chunked requests. Malformed requests receive at most 10
`400 Bad Request` responses per client address per minute; further malformed
connections are closed without a response. JSON routes have a small body
limit, validate an explicit set of fields, reject duplicate and unknown fields,
and reject unsupported JSON escapes. User values are bound as SQLite
parameters; they are not concatenated into SQL statements.

Static paths are URL-decoded and checked for traversal. Files are opened
relative to a held document-root directory, without following symlinks, and
only regular files beneath that root are served. Responses validate header
values against line breaks and encode printer names before putting them in
JSON.

Responses include `Content-Security-Policy`, `X-Content-Type-Options: nosniff`,
`X-Frame-Options: DENY`, same-origin resource policy, a restrictive
`Permissions-Policy`, `Referrer-Policy: no-referrer`, and `Cache-Control:
no-store`. The CSP allows scripts and styles only from the site's own origin,
blocks plugins and framing, and disallows inline scripts. HSTS is sent on TLS
responses only. The browser UI renders server-provided text with `textContent`
rather than treating it as HTML. These controls reduce the impact of injection;
they do not make future unsafe HTML or JavaScript changes harmless.

The application does not set CORS permission headers and authenticates API
requests with an explicit `Authorization: Bearer` header rather than an
ambient login cookie. Do not add broad CORS rules or place tokens in URLs.
Debug request logs redact authorization and cookie headers and do not log
request bodies. Request targets can appear in debug resource logs, so keep
secrets out of query strings.

## Print uploads

Print submissions require HTTPS and an active user's bearer token. The endpoint
accepts only PDF, JPEG, and PNG, checks the file signature against the declared
media type, caps uploads at 64 MiB, limits the upload duration, and spools to a
temporary file. Only one print submission runs through the service at a time.
Copies, duplex mode, and page ranges are parsed against bounded allowlists and
checked against the selected CUPS queue before job creation. The server chooses
the queue; the client cannot name an arbitrary printer.

Signature checks only establish that a file begins with the expected format
marker. They do not scan or sanitize document contents. CUPS and its document
filters still process untrusted PDF and image files; keep CUPS patched and limit
its access to trusted queues and devices. Print costs are not charged by this
endpoint yet; define pricing and atomically debit credits before treating the
service as a paid print system.

Successful submissions are saved in SQLite print history and can be read only
by the owning account through `GET /api/jobs`. The recorded status means CUPS
accepted the job. Job history refresh reads current CUPS state and active jobs
can be canceled by their owner. Request IDs embedded in CUPS job titles help
recover a missing SQLite record on retry, provided CUPS still retains the job.
This cross-system recovery is best effort and cannot provide exactly-once
printing after CUPS history is purged. Include the SQLite database in the
deployment's normal backup schedule.

## Deployment checklist

- Use a trusted CA certificate for the hostname clients actually use; do not
  rely on a self-signed development certificate for public service.
- Keep the TLS private key, runtime config, admin token, and `data/` directory
  readable only by the service administrator/account.
- Bind to loopback unless remote access is required. For remote access, use a
  firewall and a trusted certificate, and do not expose CUPS administration.
- Set a random `admin_api_token`; never commit or share it. The guided setup
  writes it to `build/local-setup/admin.token` with mode `0600`.
- Keep debug logs private and avoid placing credentials in URLs or loggable
  request targets.
- Verify phone ownership and add MFA before public signup.
- Keep CUPS, OpenSSL, SQLite, the compiler, and host operating system updated.

See the [TLS guide](tls.md), [setup guide](setup.md), and
[account and print API guide](accounts-and-print-api.md) for configuration and
request details.
