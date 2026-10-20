#!/usr/bin/env python3
"""Interactive local account manager for the server's SQLite database."""

from __future__ import annotations

import base64
import getpass
import hashlib
import os
import re
import secrets
import sqlite3
import sys
import uuid
from datetime import datetime
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "config" / "server.conf"
PBKDF2_ITERATIONS = 310_000
SALT_BYTES = 16
HASH_BYTES = 32
TOKEN_TTL_SECONDS = 30 * 24 * 60 * 60
MAX_SQL_ROWS = 1000


def configured_database() -> Path:
    """Read database_path from the simple key=value server configuration."""
    try:
        lines = CONFIG.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise RuntimeError(f"cannot read {CONFIG}: {error}") from error

    for line in lines:
        key, separator, value = line.partition("=")
        if separator and key.strip() == "database_path":
            value = value.split("#", 1)[0].strip().strip("\"'")
            if not value:
                break
            path = Path(value).expanduser()
            return path if path.is_absolute() else ROOT / path
    raise RuntimeError(f"database_path is missing from {CONFIG}")


def connect_database(path: Path) -> sqlite3.Connection:
    if path.is_symlink():
        raise RuntimeError("database path must not be a symbolic link")
    path = path.expanduser().resolve(strict=True)
    if not path.is_file():
        raise RuntimeError("database path must be an existing regular file")
    uri = f"file:{quote(os.fspath(path), safe='/')}?mode=rw"
    database = sqlite3.connect(uri, uri=True, timeout=5.0)
    database.row_factory = sqlite3.Row
    database.execute("PRAGMA foreign_keys = ON")
    database.execute("PRAGMA busy_timeout = 5000")
    version = database.execute("PRAGMA user_version").fetchone()[0]
    if version == 1:
        try:
            database.execute("BEGIN EXCLUSIVE")
            database.execute("ALTER TABLE users ADD COLUMN password_salt BLOB")
            database.execute("ALTER TABLE users ADD COLUMN password_hash BLOB")
            database.execute("PRAGMA user_version = 2")
            database.commit()
            version = 2
            print("Migrated database schema from version 1 to 2.")
        except sqlite3.Error:
            database.rollback()
            database.close()
            raise
    if version != 2:
        database.close()
        raise RuntimeError(f"unsupported database schema version {version}; expected 2")
    return database


def ask_phone() -> str:
    phone = input("Phone (E.164, e.g. +14155550123): ").strip()
    if (
        not phone.startswith("+")
        or not phone[1:].isdigit()
        or not 8 <= len(phone[1:]) <= 15
    ):
        raise ValueError("phone must use E.164 format, for example +14155550123")
    return phone


def password_digest(password: str, salt: bytes) -> bytes:
    encoded = password.encode("utf-8")
    if not 8 <= len(encoded) <= 128:
        raise ValueError("password must be 8–128 UTF-8 bytes")
    return hashlib.pbkdf2_hmac("sha256", encoded, salt, PBKDF2_ITERATIONS, HASH_BYTES)


def ask_password(prompt: str = "Password: ") -> tuple[bytes, bytes]:
    password = getpass.getpass(prompt)
    confirmation = getpass.getpass("Confirm password: ")
    if password != confirmation:
        raise ValueError("passwords do not match")
    salt = secrets.token_bytes(SALT_BYTES)
    return salt, password_digest(password, salt)


def account_row(database: sqlite3.Connection, phone: str) -> sqlite3.Row:
    row = database.execute(
        "SELECT id, phone_e164, credits, active, created_at FROM users WHERE phone_e164 = ?",
        (phone,),
    ).fetchone()
    if row is None:
        raise ValueError("no account exists for that phone number")
    return row


def display_account(row: sqlite3.Row) -> None:
    created = (
        datetime.fromtimestamp(row["created_at"])
        .astimezone()
        .isoformat(sep=" ", timespec="seconds")
    )
    print(f"\nUser ID: {row['phone_e164']}")
    print(f"Database ID: {row['id']}")
    print(f"Credits: {row['credits']}")
    print(f"Active: {'yes' if row['active'] else 'no'}")
    print(f"Created: {created}")


def lookup(database: sqlite3.Connection) -> None:
    display_account(account_row(database, ask_phone()))


def list_accounts(database: sqlite3.Connection) -> None:
    rows = database.execute(
        "SELECT id, phone_e164, credits, active, created_at FROM users "
        "ORDER BY created_at DESC, id DESC"
    ).fetchall()
    if not rows:
        print("No users found.")
        return
    print(f"{len(rows)} user(s):")
    print(f"{'PHONE / USER ID':<20} {'CREDITS':>12} {'STATUS':<10} CREATED")
    for row in rows:
        created = (
            datetime.fromtimestamp(row["created_at"])
            .astimezone()
            .isoformat(sep=" ", timespec="seconds")
        )
        status = "active" if row["active"] else "blocked"
        print(f"{row['phone_e164']:<20} {row['credits']:>12} {status:<10} {created}")


def create_account(database: sqlite3.Connection) -> None:
    phone = ask_phone()
    salt, digest = ask_password()
    with database:
        database.execute(
            "INSERT INTO users(phone_e164, credits, active, password_salt, password_hash, created_at) "
            "VALUES(?, 0, 1, ?, ?, CAST(strftime('%s', 'now') AS INTEGER))",
            (phone, salt, digest),
        )
    print("Account created.")


def reset_password(database: sqlite3.Connection) -> None:
    phone = ask_phone()
    account_row(database, phone)
    salt, digest = ask_password("New password: ")
    with database:
        database.execute(
            "UPDATE users SET password_salt = ?, password_hash = ? WHERE phone_e164 = ?",
            (salt, digest, phone),
        )
        database.execute(
            "UPDATE user_tokens SET revoked_at = CAST(strftime('%s', 'now') AS INTEGER) "
            "WHERE user_id = (SELECT id FROM users WHERE phone_e164 = ?) AND revoked_at IS NULL",
            (phone,),
        )
    print("Password changed and existing sessions revoked.")


def issue_token(database: sqlite3.Connection) -> None:
    phone = ask_phone()
    row = account_row(database, phone)
    if not row["active"]:
        raise ValueError("cannot issue a session to an inactive account")
    token = (
        base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode("ascii")
    )
    digest = hashlib.sha256(token.encode("ascii")).digest()
    with database:
        database.execute(
            "INSERT INTO user_tokens(user_id, token_hash, created_at, expires_at) "
            "VALUES(?, ?, CAST(strftime('%s', 'now') AS INTEGER), "
            "CAST(strftime('%s', 'now') AS INTEGER) + ?)",
            (row["id"], digest, TOKEN_TTL_SECONDS),
        )
    print("Copy this user token now; it cannot be recovered later:")
    print(token)


def revoke_sessions(database: sqlite3.Connection) -> None:
    phone = ask_phone()
    account_row(database, phone)
    with database:
        cursor = database.execute(
            "UPDATE user_tokens SET revoked_at = CAST(strftime('%s', 'now') AS INTEGER) "
            "WHERE user_id = (SELECT id FROM users WHERE phone_e164 = ?) AND revoked_at IS NULL",
            (phone,),
        )
    print(f"Revoked {cursor.rowcount} session(s).")


def adjust_active(database: sqlite3.Connection, active: bool) -> None:
    phone = ask_phone()
    account_row(database, phone)
    with database:
        database.execute(
            "UPDATE users SET active = ? WHERE phone_e164 = ?", (int(active), phone)
        )
        if not active:
            database.execute(
                "UPDATE user_tokens SET revoked_at = CAST(strftime('%s', 'now') AS INTEGER) "
                "WHERE user_id = (SELECT id FROM users WHERE phone_e164 = ?) AND revoked_at IS NULL",
                (phone,),
            )
    print(f"Account {'activated' if active else 'deactivated'}.")


def add_credits(database: sqlite3.Connection) -> None:
    phone = ask_phone()
    row = account_row(database, phone)
    amount = int(input("Credits to add (positive whole number): ").strip())
    if not 0 < amount <= 2**63 - 1 or row["credits"] > 2**63 - 1 - amount:
        raise ValueError(
            "credit amount is invalid or would overflow the account balance"
        )
    note = input("Ledger note (optional): ").strip()[:512] or None
    reference = f"local-admin:{uuid.uuid4()}"
    balance = row["credits"] + amount
    with database:
        database.execute(
            "UPDATE users SET credits = ? WHERE id = ?", (balance, row["id"])
        )
        database.execute(
            "INSERT INTO credit_ledger(user_id, amount, balance_after, entry_type, reference, note, created_at) "
            "VALUES(?, ?, ?, 'topup', ?, ?, CAST(strftime('%s', 'now') AS INTEGER))",
            (row["id"], amount, balance, reference, note),
        )
    print(f"Added {amount} credits. New balance: {balance}.")


def sql_console(database: sqlite3.Connection) -> None:
    print("Direct SQL console for the configured database.")
    print("Use one SQL statement. Reads are displayed; changes require typing APPLY.")
    print(
        "PRAGMA, ATTACH/DETACH, VACUUM, and transaction-control statements are blocked."
    )
    sql = input("SQL> ").strip()
    if not sql:
        return
    statement = re.sub(
        r"\A(?:\s|--[^\n]*(?:\n|$)|/\*.*?\*/)*", "", sql, flags=re.DOTALL
    )
    first_word = statement.split(None, 1)[0].rstrip(";").upper()
    blocked = {
        "PRAGMA",
        "ATTACH",
        "DETACH",
        "VACUUM",
        "BEGIN",
        "COMMIT",
        "ROLLBACK",
        "SAVEPOINT",
        "RELEASE",
    }
    if first_word in blocked:
        raise ValueError(f"{first_word} is not available in the SQL console")
    database.execute("BEGIN IMMEDIATE")
    try:
        cursor = database.execute(sql)
        if first_word in {"SELECT", "EXPLAIN"}:
            if cursor.description is None:
                raise ValueError("expected a read-only query")
            rows = cursor.fetchmany(MAX_SQL_ROWS + 1)
            for row in rows[:MAX_SQL_ROWS]:
                print(dict(row))
            if len(rows) > MAX_SQL_ROWS:
                print(f"Output truncated at {MAX_SQL_ROWS} rows.")
            database.rollback()
            return
        print(f"Statement affected {cursor.rowcount} row(s).")
        if cursor.description is not None:
            for row in cursor.fetchmany(MAX_SQL_ROWS):
                print(dict(row))
        if input("Type APPLY to commit these changes: ").strip() == "APPLY":
            database.commit()
            print("Changes committed.")
        else:
            database.rollback()
            print("Changes discarded.")
    except sqlite3.Error:
        database.rollback()
        raise


MENU = """
1) Look up account
2) Create account
3) Reset password (revokes sessions)
4) Issue 30-day user token
5) Revoke all user sessions
6) Add credits
7) Activate account
8) Blocklist user (deactivates account and revokes sessions)
9) Direct SQL console
10) List all users
0) Quit
"""


def main() -> int:
    try:
        path = (
            Path(sys.argv[1]).expanduser()
            if len(sys.argv) > 1
            else configured_database()
        )
        if len(sys.argv) > 2:
            raise RuntimeError("usage: python3 scripts/admin_cli.py [database-path]")
        if not path.is_absolute():
            path = ROOT / path
        database = connect_database(path)
    except (OSError, RuntimeError, sqlite3.Error) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    print(f"Managing local database: {path.resolve()}")
    actions = {
        "1": lookup,
        "2": create_account,
        "3": reset_password,
        "4": issue_token,
        "5": revoke_sessions,
        "6": add_credits,
        "7": lambda db: adjust_active(db, True),
        "8": lambda db: adjust_active(db, False),
        "9": sql_console,
        "10": list_accounts,
    }
    try:
        while True:
            print(MENU)
            choice = input("Select an option: ").strip()
            if choice == "0":
                return 0
            action = actions.get(choice)
            if action is None:
                print("Choose one of the listed options.")
                continue
            try:
                action(database)
            except (ValueError, sqlite3.Error) as error:
                print(f"Error: {error}", file=sys.stderr)
    except (EOFError, KeyboardInterrupt):
        print("\nExiting.")
        return 0
    finally:
        database.close()


if __name__ == "__main__":
    raise SystemExit(main())
