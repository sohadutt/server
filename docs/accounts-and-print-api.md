# Accounts, credits, and print API

## User and credit storage

`include/data.h` and `src/storage/data.c` provide SQLite account and credit
storage. The database stores E.164 phone IDs, active status,
PBKDF2-HMAC-SHA256 password hashes, token digests, credit balances, and an
append-only credit ledger. Existing schema version 1 databases migrate
automatically, and are upgraded to schema version 5 when opened.

The token API returns a newly generated bearer token only when issuing it; the
database stores only its SHA-256 digest. Tokens cannot be recovered from the
database and expire after 30 days. Deactivating a user blocks their tokens
while retaining their account and ledger history.

Balances are signed 64-bit integer credit units, never floating-point money.
`data_credit_add()` should only be called after a future payment flow has
verified a payment, and `data_credit_charge()` atomically checks and debits a
balance. A non-null reference is unique per user, so retries of a payment or
print charge can be detected instead of applying the same ledger event twice.
The module does not process payments, choose a price per page, or deduct
credits for print jobs. Define pricing and payment verification before
enabling credit charging in a job workflow.

Accepted print submissions are recorded with an application job ID, a batch
ID, an idempotency key, the document name, CUPS job ID, assigned queue,
submitted format, copies, duplex mode, requested page ranges, and submission
time. The browser can submit up to 10 files in one grouped batch; each file
becomes its own CUPS job so it can have different copies, duplex mode, and PDF
page ranges. Retries with the same request ID return the existing job when its
database record is present. If a grouped submission partially succeeds,
successful files remain in history and only failed files need to be retried.
`GET /api/jobs` returns up to the most recent 50 records and refreshes their
status from CUPS. `POST /api/jobs/{id}/cancel` cancels a pending or processing
job belonging to the signed-in account. Status refresh happens when the user
opens or refreshes job history. A CUPS job title includes the internal user ID
and request ID so a retry can restore a missing SQLite row if the process
stopped after CUPS accepted the job. This recovery depends on CUPS retaining
job history; it is best effort, not an atomic transaction across CUPS and
SQLite, and cannot guarantee exactly-once printing after CUPS history is
purged. Credit charging remains disabled until pricing and a failure/refund
policy are defined.

By default, the first submitted document receives a QR code on its first page.
It contains the account's numeric database ID and a small visible ID label; it
does not include the phone number because printouts can be shared or discarded.
The QR is an operational lookup hint only: it is not signed, cannot prove the
owner's identity, and must never be accepted as an authentication credential.
The default QR is 12 mm; the advanced menu can disable it or change its
corner and size (16 or 20 mm). When a
PDF page range is selected, the QR is placed on the first selected page.
The browser creates the QR and prepares a stamped PDF before upload. It uses
self-hosted versions of pdf-lib and qrcode-generator, so it does not call a
third-party service or require PDF tools on the server. Images are converted to
PDF in the browser. The QR is inset 10 mm from the page edge, which may still
be inside some printers' unprintable margins.

The server opens `database_path` at startup and creates its immediate parent
directory with owner-only permissions if it is missing. SQLite uses WAL,
foreign keys, a busy timeout, full synchronous writes, secure deletion, and
0600 permissions on the database file.

## Local account administration

The local administrator tool opens the configured SQLite database directly.
It does not connect to the server or ask for an API secret:

```sh
python3 scripts/admin_cli.py
```

Pass another existing database path as an argument to manage it. The tool
supports account lookup/listing/creation, password reset, token issue and
revocation, credit additions, activation/blocklisting, and a direct SQL
console. Blocklisting deactivates the account and revokes its active sessions.
SQL changes require typing `APPLY` before committing.

Access to this tool is controlled by the operating system account that can
read and write the database. Run it only on a trusted machine and keep the
`data` directory private. The server's HTTP admin routes remain separately
protected by `admin_api_token`; that setting is not used by this script.

## Browser login and HTTP admin routes

For administration through the web API, set `admin_api_token` in
`config/server.conf` to a private random value of at least 32 characters.
Generate one with `openssl rand -hex 32`; keep it private and do not commit it.
Admin requests use `Authorization: Bearer <admin-secret>`. All account, admin,
and print API routes require HTTPS; HTTP requests receive `426 Upgrade
Required`. Responses use a restrictive Content Security Policy, disable MIME
sniffing and framing, and disable caching. The browser keeps the user session
token in memory only, so reloading the page requires signing in again.
Login, signup, and admin routes are throttled per client address (12, 5, and
30 requests per 60-second window, respectively). Forwarded client addresses
are considered only when the TCP peer is loopback, for a local reverse proxy.
Other API traffic is capped at 300 requests per address per minute. Each
verified active bearer session is separately capped at 120 API requests and 3
print submissions per minute. Session limits key a SHA-256 digest of the
token, never the token itself. Repeated malformed HTTP requests are limited to
10 replies per address per minute; further malformed connections are silently
closed.

Create an account:

```sh
curl --fail-with-body --cacert certs/server.crt \
  -X POST https://localhost:8443/api/admin/users \
  -H 'Authorization: Bearer YOUR_ADMIN_SECRET' \
  -H 'Content-Type: application/json' \
  -d '{"phone":"+14155550123","password":"use-a-unique-password"}'
```

Reset a forgotten password:

```sh
curl --fail-with-body --cacert certs/server.crt \
  -X POST https://localhost:8443/api/admin/users/password \
  -H 'Authorization: Bearer YOUR_ADMIN_SECRET' \
  -H 'Content-Type: application/json' \
  -d '{"phone":"+14155550123","new_password":"another-unique-password"}'
```

Passwords must contain 8–128 characters. Reset invalidates the user's
existing sessions. Outside local development, send admin secrets and user
passwords only over HTTPS.

The print page at `/print/` supports sign-in using a phone number and password.
Signup is disabled by default (`signup_enabled = false`) until a phone
ownership verification provider is configured. Enabling signup in a trusted
development environment does not prove the requester owns the phone number.
Administrators can create accounts through the protected admin route or local
CLI. Login returns
a random session token; the browser keeps it in memory and uses it for print
requests. `GET /api/me` returns the signed-in phone and balance;
`POST /api/logout` revokes the current session. The print endpoint requires
an active user session. Phone ownership is not verified yet; add SMS
verification before open signup requires confirmed phone ownership.

| Route | Method | Purpose |
| --- | --- | --- |
| `/api/signup` | `POST` | Creates an account from `phone` and `password`, then returns a session |
| `/api/login` | `POST` | Accepts phone and password and returns a session token and balance |
| `/api/me` | `GET` | Returns the signed-in phone and current balance |
| `/api/jobs` | `GET` | Returns the signed-in user's recent print jobs and refreshes CUPS status |
| `/api/jobs/{id}/cancel` | `POST` | Cancels one of the signed-in user's active jobs |
| `/api/logout` | `POST` | Revokes the current user session |
| `/api/admin/users` | `POST` | Admin-only account creation with phone and password |
| `/api/admin/users/password` | `POST` | Admin-only password reset; revokes all sessions |
| `/api/admin/users/lookup` | `POST` | Admin-only lookup by phone ID |
| `/api/admin/users/token` | `POST` | Issues a one-time, 30-day user token |
| `/api/admin/users/token/revoke` | `POST` | Revokes a specific user's session token |
| `/api/admin/users/revoke` | `POST` | Revokes every session for the phone ID |
| `/api/admin/users/credits` | `POST` | Adds credits with a ledger entry and optional unique reference |
| `/api/admin/users/active` | `POST` | Activates/deactivates an account; deactivation revokes sessions |
| `/api/print` | `POST` | Submits a document using the signed-in user's bearer token |

## Print document request

`POST /api/print` takes the document as the raw request body, not base64 or a
JSON manifest. It accepts PDF, JPEG, and PNG uploads up to 64 MiB. Requests
must include `Authorization: Bearer <token>` and `Content-Type` set to the
document's media type. Optional settings are sent as headers:

| Header | Values | Default |
| --- | --- | --- |
| `X-Print-Copies` | `1`–`99` | `1` |
| `X-Print-Sides` | `one-sided`, `two-sided-long-edge`, `two-sided-short-edge` | `one-sided` |
| `X-Print-Pages` | PDF only, for example `1-3,5` | All pages |

The endpoint requires HTTPS and an active account session. It checks the file
signature against the declared format, caps each upload at 64 MiB, spools one
request at a time to an unlinked temporary file, and validates requested print
options against the selected queue before submission. It chooses an eligible
configured CUPS queue using active-job counts, then streams the document to
CUPS. Its JSON response includes the assigned printer and CUPS job ID.

```sh
curl --fail-with-body \
  -H 'Authorization: Bearer USER_SESSION_TOKEN' \
  -H 'Content-Type: application/pdf' \
  -H 'X-Print-Copies: 2' \
  -H 'X-Print-Sides: two-sided-long-edge' \
  -H 'X-Print-Pages: 1-3,5' \
  --data-binary @document.pdf \
  --cacert certs/server.crt \
  https://localhost:8443/api/print
```

The browser page accepts up to 10 documents and exposes settings per document
in a mobile-friendly form. The files are sent sequentially under a shared batch
ID, and successful files remain visible if a later file fails. Retry only the
failed documents; the request ID avoids resubmitting jobs that were already
recorded. Printer selection stays server-side so a client cannot force a job
onto a specific queue.
