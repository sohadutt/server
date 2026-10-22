# Administrator guide

## Start CUPS and configure printer queues

From the repository root:

```sh
./scripts/setup.sh cups start
./scripts/setup.sh cups status
./scripts/setup.sh cups discover
./scripts/setup.sh cups add-printer
```

Starting CUPS may ask for administrator privileges. Discovery lists IPP
printers visible to CUPS; add a device as a driverless queue before the server
can send jobs to it. `cups status` shows configured queues and the default
printer.

For each incoming print request, the service skips queues that are stopped or
rejecting jobs. Among eligible queues, it chooses one with the fewest active
jobs, preferring an idle queue, then the CUPS default, then the alphabetically
first queue to break ties. If CUPS cannot refresh queue/job status, the server
does not assign a printer.

The server's `cups_server` and `cups_encryption_required` settings select how
it connects to the scheduler. They do not create queues or change printer
hardware settings.

## Manage local accounts with the CLI

Run the interactive manager on the host that stores the SQLite database:

```sh
python3 scripts/admin_cli.py
```

The tool reads `database_path` from `config/server.conf`. To manage another
database, supply its path:

```sh
python3 scripts/admin_cli.py /path/to/server.sqlite
```

The database must already exist and use schema version 5. Start the current
server once to migrate an older database before running this manager. Access is
controlled by operating-system file permissions; the CLI does not ask for an
HTTP admin token.

Menu actions include:

1. Look up one account by phone number.
2. Create an account and set its password.
3. Reset a password and revoke all existing sessions.
4. Issue a one-time 30-day user token.
5. Revoke all sessions for an account.
6. Add credits with a ledger entry and optional note.
7. Activate an account.
8. Blocklist an account, which also revokes its active sessions.
9. Run one SQL statement in the guarded local console.
10. List all users.

Phone numbers use E.164 format, such as `+14155550123`. Passwords must be 8
through 128 UTF-8 bytes. An issued token is displayed once; copy it securely
because the database stores only its digest. Direct SQL changes require typing
`APPLY` to commit. Keep the database directory private because it contains
phone numbers, password hashes, account balances, sessions, and print history.

## Admin HTTP endpoints

The server also supports administrative HTTP requests for account creation,
password reset, account lookup, token issue/revocation, credit adjustment, and
account activation. Configure `admin_api_token` in a private server
configuration and send it as `Authorization: Bearer <secret>`. Generate a
secret with:

```sh
openssl rand -hex 32
```

Use these endpoints only over HTTPS. The generated secret is not the same as a
user's phone/password or session token. The endpoint list and JSON examples
are in [Accounts, credits, and print API](../accounts-and-print-api.md).

## Credits and billing behavior

Credit balances are integer units and all additions are written to a ledger.
The local CLI can add credits, but the server does not currently charge credits
when printing or accept payments. Choose pricing, payment verification, and a
refund policy before enabling print charges. Do not treat a manual credit
addition as confirmation that payment has been received.

## Account recovery

For a forgotten password, use menu option 3 or the admin password-reset
endpoint. Resetting a password revokes existing sessions. To block an account,
use menu option 8; this deactivates the account and revokes active sessions.
Use menu option 7 to reactivate it later.
