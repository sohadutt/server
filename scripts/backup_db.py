#!/usr/bin/env python3
"""Create a consistent, private SQLite backup while the server is running."""

from __future__ import annotations

import argparse
from contextlib import closing
import os
from pathlib import Path
import sqlite3
import tempfile
from datetime import datetime, timezone
from urllib.parse import quote


def backup_database(source_path: Path, backup_dir: Path) -> Path:
    source_path = source_path.resolve(strict=True)
    if not source_path.is_file():
        raise ValueError(f"Database is not a regular file: {source_path}")

    backup_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    if backup_dir.is_symlink() or not backup_dir.is_dir():
        raise ValueError(f"Backup destination is not a directory: {backup_dir}")
    directory_status = backup_dir.stat()
    if directory_status.st_uid != os.getuid() or directory_status.st_mode & 0o077:
        raise ValueError("Backup directory must be owned by you and private (mode 0700)")

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    destination = backup_dir / f"server-{timestamp}.sqlite"
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".server-backup-", suffix=".sqlite", dir=backup_dir
    )
    os.fchmod(descriptor, 0o600)
    os.close(descriptor)
    temporary_path = Path(temporary_name)

    source_uri = f"file:{quote(str(source_path), safe='/')}?mode=ro"
    try:
        with closing(sqlite3.connect(source_uri, uri=True, timeout=30)) as source:
            with closing(sqlite3.connect(temporary_path, timeout=30)) as target:
                source.backup(target, pages=256, sleep=0.05)
                check = target.execute("PRAGMA integrity_check;").fetchone()
                if check != ("ok",):
                    raise RuntimeError(f"SQLite integrity check failed: {check}")
        temporary_path.chmod(0o600)
        with temporary_path.open("rb") as backup_file:
            os.fsync(backup_file.fileno())
        os.replace(temporary_path, destination)
        directory_fd = os.open(backup_dir, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create a consistent SQLite backup without stopping the server."
    )
    parser.add_argument("database", nargs="?", type=Path,
                        default=Path("data/server.sqlite"))
    parser.add_argument("backup_dir", nargs="?", type=Path,
                        default=Path("backups"))
    arguments = parser.parse_args()
    try:
        backup = backup_database(arguments.database, arguments.backup_dir)
    except (OSError, sqlite3.Error, RuntimeError, ValueError) as error:
        parser.exit(1, f"Backup failed: {error}\n")
    print(backup)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
