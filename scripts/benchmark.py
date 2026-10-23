#!/usr/bin/env python3
"""Ramp-load benchmark for the C server (Python standard library only)."""

from __future__ import annotations

import argparse
import asyncio
import errno
import hashlib
import os
import secrets
import sqlite3
import ssl
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import quote, urlsplit

ROOT = Path(__file__).resolve().parent.parent
MAX_LATENCY_SAMPLES = 100_000


def config_value(name: str) -> str:
    config = ROOT / "config" / "server.conf"
    for line in config.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == name:
            return value.split("#", 1)[0].strip().strip("\"'")
    raise ValueError(f"{name} is missing from {config}")


def database_path(argument: str | None) -> Path:
    path = (
        Path(argument).expanduser()
        if argument
        else Path(config_value("database_path")).expanduser()
    )
    if not path.is_absolute():
        path = ROOT / path
    if path.is_symlink():
        raise ValueError("database path must not be a symbolic link")
    path = path.resolve(strict=True)
    if not path.is_file():
        raise ValueError("database path must be an existing regular file")
    return path


def connect_db(path: Path) -> sqlite3.Connection:
    uri = f"file:{quote(os.fspath(path), safe='/')}?mode=rw"
    db = sqlite3.connect(uri, uri=True, timeout=10.0)
    db.execute("PRAGMA busy_timeout = 10000")
    version = db.execute("PRAGMA user_version").fetchone()[0]
    if version != 2:
        db.close()
        raise ValueError(f"unsupported database schema version {version}; expected 2")
    return db


def resolve_pid(explicit_pid: int | None, port: int) -> int:
    if explicit_pid is not None:
        try:
            os.kill(explicit_pid, 0)
        except OSError as error:
            raise ValueError(
                f"server PID {explicit_pid} is not running: {error}"
            ) from error
        return explicit_pid
    try:
        result = subprocess.run(
            ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-t"],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError("could not find server PID; pass --pid explicitly") from error
    pids = sorted({int(value) for value in result.stdout.split() if value.isdigit()})
    if len(pids) != 1:
        raise ValueError(
            f"expected one process listening on port {port}, found {pids or 'none'}; "
            "pass --pid to measure the server process"
        )
    return pids[0]


def read_rss_mib(pid: int) -> float | None:
    try:
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)],
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
        value = result.stdout.strip()
        return int(value) / 1024.0 if result.returncode == 0 and value else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def prepare_tokens(
    db: sqlite3.Connection, phone: str, count: int
) -> tuple[list[str], list[int]]:
    user = db.execute(
        "SELECT id FROM users WHERE phone_e164 = ? AND active = 1", (phone,)
    ).fetchone()
    if user is None:
        raise ValueError("phone must belong to an existing active user")
    tokens: list[str] = []
    row_ids: list[int] = []
    now = int(time.time())
    try:
        db.execute("BEGIN IMMEDIATE")
        for _ in range(count):
            token = secrets.token_urlsafe(32)
            digest = hashlib.sha256(token.encode("ascii")).digest()
            cursor = db.execute(
                "INSERT INTO user_tokens(user_id, token_hash, created_at, expires_at) "
                "VALUES(?, ?, ?, ?)",
                (user[0], digest, now, now + 3600),
            )
            tokens.append(token)
            row_ids.append(cursor.lastrowid)
        db.commit()
    except sqlite3.Error:
        db.rollback()
        raise
    return tokens, row_ids


def revoke_tokens(db: sqlite3.Connection, row_ids: list[int]) -> None:
    if not row_ids:
        return
    try:
        db.execute("BEGIN IMMEDIATE")
        for offset in range(0, len(row_ids), 400):
            batch = row_ids[offset : offset + 400]
            marks = ",".join("?" for _ in batch)
            db.execute(
                "UPDATE user_tokens SET revoked_at = ? "
                f"WHERE id IN ({marks}) AND revoked_at IS NULL",
                (int(time.time()), *batch),
            )
        db.commit()
    except sqlite3.Error:
        db.rollback()
        raise


def ramp_levels(maximum: int, step: int) -> list[int]:
    levels = list(range(step, maximum, step))
    if not levels or levels[-1] != maximum:
        levels.append(maximum)
    return levels


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * pct))
    return ordered[index]


@dataclass
class StageStats:
    concurrency: int
    started: float = field(default_factory=time.monotonic)
    elapsed: float = 0.0
    completed: int = 0
    failed: int = 0
    status_counts: dict[int, int] = field(default_factory=dict)
    failure_reasons: dict[str, int] = field(default_factory=dict)
    latency_samples: list[float] = field(default_factory=list)
    rss_samples: list[float] = field(default_factory=list)
    active_requests: int = 0
    peak_active_requests: int = 0
    open_connections: int = 0
    peak_open_connections: int = 0

    def connection_opened(self) -> None:
        self.open_connections += 1
        self.peak_open_connections = max(
            self.peak_open_connections, self.open_connections
        )

    def connection_closed(self) -> None:
        self.open_connections = max(0, self.open_connections - 1)

    def record(
        self, status: int | None, elapsed: float, failure_reason: str | None
    ) -> None:
        if status == 200:
            self.completed += 1
        else:
            self.failed += 1
            if status is not None:
                self.status_counts[status] = self.status_counts.get(status, 0) + 1
            reason = failure_reason or (
                f"http_{status}" if status is not None else "unknown"
            )
            self.failure_reasons[reason] = self.failure_reasons.get(reason, 0) + 1
        # Bound client-side memory even during long, high-throughput runs.
        if len(self.latency_samples) < MAX_LATENCY_SAMPLES:
            self.latency_samples.append(elapsed)


async def request_once(
    host: str,
    port: int,
    path: str,
    token: str | None,
    timeout: float,
    tls_context: ssl.SSLContext | None,
    stats: StageStats | None = None,
) -> tuple[int | None, float, str | None]:
    started = time.monotonic()
    writer: asyncio.StreamWriter | None = None
    phase = "connect"
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(
                host,
                port,
                ssl=tls_context,
                server_hostname=host if tls_context else None,
            ),
            timeout=timeout,
        )
        if stats is not None:
            stats.connection_opened()
        headers = [
            f"GET {path} HTTP/1.1",
            f"Host: {host}",
            "Connection: close",
            "Accept: */*",
        ]
        if token is not None:
            headers.append(f"Authorization: Bearer {token}")
        phase = "write"
        writer.write(("\r\n".join(headers) + "\r\n\r\n").encode("ascii"))
        await asyncio.wait_for(writer.drain(), timeout=timeout)
        phase = "response_status"
        status_line = await asyncio.wait_for(reader.readline(), timeout=timeout)
        if not status_line:
            return None, time.monotonic() - started, "empty_response"
        pieces = status_line.decode("ascii", errors="replace").split()
        if len(pieces) < 2 or not pieces[1].isdigit():
            # Include a short escaped sample: it distinguishes an empty close,
            # a TLS/proxy preamble, and malformed HTTP without dumping a body.
            sample = repr(status_line[:80])
            return (
                None,
                time.monotonic() - started,
                f"invalid_status_line:{sample}",
            )
        status = int(pieces[1])
        # Drain the response so the client measures complete requests.
        phase = "response_body"
        while await asyncio.wait_for(reader.read(65536), timeout=timeout):
            pass
        return status, time.monotonic() - started, None
    except asyncio.TimeoutError:
        return None, time.monotonic() - started, f"timeout_{phase}"
    except ssl.SSLError as error:
        return None, time.monotonic() - started, f"tls_{type(error).__name__}"
    except OSError as error:
        error_name = errno.errorcode.get(error.errno, type(error).__name__)
        return None, time.monotonic() - started, f"{phase}_{error_name}"
    except asyncio.IncompleteReadError:
        return None, time.monotonic() - started, f"incomplete_{phase}"
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except (OSError, asyncio.TimeoutError):
                pass
            # Let the selector retire this transport before a rapid reconnect
            # reuses its file descriptor (notably on recent macOS/Python builds).
            await asyncio.sleep(0)
            if stats is not None:
                stats.connection_closed()


class PersistentHTTPClient:
    """Sequential HTTP/1.1 client that reuses keep-alive connections."""

    def __init__(
        self, host: str, port: int, path: str, token: str | None,
        timeout: float, tls_context: ssl.SSLContext | None,
        stats: StageStats,
    ) -> None:
        self.host = host
        self.port = port
        self.path = path
        self.token = token
        self.timeout = timeout
        self.tls_context = tls_context
        self.stats = stats
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None

    async def close(self) -> None:
        writer, self.writer = self.writer, None
        self.reader = None
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except (OSError, asyncio.TimeoutError):
                pass
            await asyncio.sleep(0)
            self.stats.connection_closed()

    async def request(self) -> tuple[int | None, float, str | None]:
        started = time.monotonic()
        phase = "connect"
        try:
            if self.reader is None or self.writer is None:
                self.reader, self.writer = await asyncio.wait_for(
                    asyncio.open_connection(
                        self.host, self.port, ssl=self.tls_context,
                        server_hostname=self.host if self.tls_context else None,
                    ),
                    timeout=self.timeout,
                )
                self.stats.connection_opened()
            headers = [
                f"GET {self.path} HTTP/1.1",
                f"Host: {self.host}",
                "Connection: keep-alive",
                "Accept: */*",
            ]
            if self.token is not None:
                headers.append(f"Authorization: Bearer {self.token}")
            phase = "write"
            self.writer.write(("\r\n".join(headers) + "\r\n\r\n").encode("ascii"))
            await asyncio.wait_for(self.writer.drain(), timeout=self.timeout)

            phase = "response_status"
            status_line = await asyncio.wait_for(
                self.reader.readline(), timeout=self.timeout
            )
            pieces = status_line.decode("ascii", errors="replace").split()
            if len(pieces) < 2 or not pieces[1].isdigit():
                sample = repr(status_line[:80])
                await self.close()
                return None, time.monotonic() - started, f"invalid_status_line:{sample}"
            status = int(pieces[1])

            phase = "response_headers"
            content_length: int | None = None
            connection_close = False
            while True:
                line = await asyncio.wait_for(
                    self.reader.readline(), timeout=self.timeout
                )
                if line in (b"\r\n", b"\n"):
                    break
                if not line:
                    raise ConnectionError("connection ended inside response headers")
                name, separator, value = line.partition(b":")
                if not separator:
                    raise ValueError("malformed response header")
                if name.strip().lower() == b"content-length":
                    content_length = int(value.strip())
                elif name.strip().lower() == b"connection":
                    connection_close = value.strip().lower() == b"close"
            if content_length is None:
                raise ValueError("response has no Content-Length")

            phase = "response_body"
            await asyncio.wait_for(
                self.reader.readexactly(content_length), timeout=self.timeout
            )
            if connection_close:
                await self.close()
            return status, time.monotonic() - started, None
        except asyncio.TimeoutError:
            await self.close()
            return None, time.monotonic() - started, f"timeout_{phase}"
        except ssl.SSLError as error:
            await self.close()
            return None, time.monotonic() - started, f"tls_{type(error).__name__}"
        except (OSError, asyncio.IncompleteReadError, ValueError, ConnectionError) as error:
            await self.close()
            name = errno.errorcode.get(getattr(error, "errno", None), type(error).__name__)
            return None, time.monotonic() - started, f"{phase}_{name}"


async def run_stage(
    *,
    level: int,
    duration: float,
    host: str,
    port: int,
    path: str,
    tokens: list[str],
    timeout: float,
    tls_context: ssl.SSLContext | None,
    pid: int,
    connection_mode: str,
) -> StageStats:
    stats = StageStats(concurrency=level)
    start_workers = asyncio.Event()
    deadline = 0.0
    stop_sampler = asyncio.Event()

    async def sample_rss() -> None:
        while not stop_sampler.is_set():
            rss = await asyncio.to_thread(read_rss_mib, pid)
            if rss is not None:
                stats.rss_samples.append(rss)
            try:
                await asyncio.wait_for(stop_sampler.wait(), timeout=0.5)
            except asyncio.TimeoutError:
                pass

    async def worker(worker_id: int) -> None:
        token = tokens[worker_id] if tokens else None
        persistent_client = (
            PersistentHTTPClient(host, port, path, token, timeout, tls_context, stats)
            if connection_mode == "keep-alive"
            else None
        )
        try:
            await start_workers.wait()
            while time.monotonic() < deadline:
                stats.active_requests += 1
                stats.peak_active_requests = max(
                    stats.peak_active_requests, stats.active_requests
                )
                try:
                    if persistent_client is not None:
                        result = await persistent_client.request()
                    else:
                        result = await request_once(
                            host, port, path, token, timeout, tls_context, stats
                        )
                finally:
                    stats.active_requests -= 1
                stats.record(*result)
        finally:
            if persistent_client is not None:
                await persistent_client.close()

    sampler = asyncio.create_task(sample_rss())
    workers = [asyncio.create_task(worker(index)) for index in range(level)]
    try:
        stats.started = time.monotonic()
        deadline = stats.started + duration
        start_workers.set()
        await asyncio.gather(*workers)
    finally:
        stop_sampler.set()
        await sampler
        stats.elapsed = time.monotonic() - stats.started
    return stats


def print_stage(stats: StageStats) -> tuple[float, float]:
    total = stats.completed + stats.failed
    duration = stats.elapsed if stats.elapsed > 0 else 1.0
    attempted_rps = total / duration
    successful_rps = stats.completed / duration
    latencies = stats.latency_samples
    p95 = percentile(latencies, 0.95) * 1000
    peak_rss = max(stats.rss_samples, default=0.0)
    avg_rss = statistics.fmean(stats.rss_samples) if stats.rss_samples else 0.0
    print(
        f"{stats.concurrency:>5} clients | {successful_rps:>9.1f} ok/s | "
        f"{attempted_rps:>9.1f} attempted/s | "
        f"{stats.completed:>8} ok | {stats.failed:>6} failed | "
        f"p50 {percentile(latencies, 0.50) * 1000:>7.1f} ms | "
        f"p95 {p95:>7.1f} ms | RSS avg/peak {avg_rss:.1f}/{peak_rss:.1f} MiB"
    )
    print(
        f"      Client workers: {stats.concurrency}; peak active requests: "
        f"{stats.peak_active_requests}; peak established connections: "
        f"{stats.peak_open_connections}"
    )
    if stats.status_counts:
        print(f"      Non-200 HTTP statuses: {stats.status_counts}")
    if stats.failure_reasons:
        print(f"      Failure reasons: {stats.failure_reasons}")
    return successful_rps, p95


async def run_benchmark(
    args: argparse.Namespace, parsed, pid: int, tokens: list[str]
) -> int:
    host = parsed.hostname
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    if parsed.scheme == "https":
        ca_file = args.ca_cert if Path(args.ca_cert).is_file() else None
        tls_context = ssl.create_default_context(cafile=ca_file)
    else:
        tls_context = None
    path = args.path
    if not path.startswith("/") or "\r" in path or "\n" in path:
        raise ValueError("--path must start with / and contain no line breaks")

    probe_status, _, probe_error = await request_once(
        host,
        port,
        path,
        tokens[0] if tokens else None,
        args.timeout,
        tls_context,
    )
    if probe_status != 200:
        detail = probe_error or f"HTTP {probe_status}"
        raise ValueError(
            f"preflight request to {path} did not return HTTP 200 ({detail}); "
            "choose a valid static path or a successful API endpoint"
        )

    baseline = read_rss_mib(pid)
    if baseline is None:
        raise ValueError(f"could not read RSS for PID {pid}")
    levels = ramp_levels(args.max_concurrency, args.step)
    print(f"Target: {args.url.rstrip('/')}{path}")
    print(f"Server PID: {pid}; baseline RSS: {baseline:.1f} MiB")
    print(
        f"Load: {levels[0]} to {levels[-1]} concurrent clients, {args.stage_duration:g}s per stage"
    )
    print(f"Connection mode: {args.connection_mode}")
    print(
        "Latency covers each request coroutine from connect (when needed) "
        "through the complete response body; it excludes time between requests. "
        "The benchmark launches exactly the configured number of sequential "
        "client worker tasks; established sockets may be fewer while "
        "connects/TLS handshakes are in progress."
    )
    if tokens:
        print(f"Temporary authenticated sessions: {len(tokens)}")
    print("\nConcurrency | Successful/s | Attempted/s | Successful | Failed | Latency | RSS")

    total_ok = total_failed = 0
    best_rps = 0.0
    previous_rps: float | None = None
    previous_p95: float | None = None
    saturation_level: int | None = None
    for level in levels:
        stage_tokens = tokens[:level] if tokens else []
        stats = await run_stage(
            level=level,
            duration=args.stage_duration,
            host=host,
            port=port,
            path=path,
            tokens=stage_tokens,
            timeout=args.timeout,
            tls_context=tls_context,
            pid=pid,
            connection_mode=args.connection_mode,
        )
        rps, p95 = print_stage(stats)
        total_ok += stats.completed
        total_failed += stats.failed
        throughput_plateau = (
            previous_rps is not None
            and rps <= previous_rps * 1.10
            and previous_p95 is not None
            and p95 >= previous_p95 * 1.5
        )
        if rps > best_rps:
            best_rps = rps
        if saturation_level is None and (
            stats.failed > 0
            or (best_rps > 0 and rps < best_rps * 0.9)
            or throughput_plateau
        ):
            saturation_level = level
        previous_rps = rps
        previous_p95 = p95
        if args.stop_on_saturation and saturation_level is not None:
            print(f"Stopping ramp: saturation signal at {level} clients.")
            break

    print("\nSummary")
    print(
        f"Requests: {total_ok + total_failed} ({total_ok} successful, {total_failed} failed)"
    )
    print(f"Best successful throughput: {best_rps:.1f} requests/second")
    print(
        f"First saturation signal: {saturation_level or 'not observed'} concurrent clients"
    )
    print(
        "Saturation is flagged by failures, a throughput drop over 10%, or throughput plateauing while p95 latency jumps 50%."
    )
    return 0 if total_failed == 0 else 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Ramp concurrent load until the server shows errors or throughput saturation."
    )
    parser.add_argument(
        "--url", default="https://localhost:8443", help="server origin URL"
    )
    parser.add_argument(
        "--ca-cert",
        default=(ROOT / "certs" / "server.crt").as_posix(),
        help="CA/certificate file used to verify HTTPS",
    )
    parser.add_argument(
        "--path",
        default="/",
        help="GET path; use /api/me for authenticated database load",
    )
    parser.add_argument(
        "--phone", help="existing active test-user phone; required for /api/me"
    )
    parser.add_argument(
        "--database", help="SQLite database path; defaults to server.conf"
    )
    parser.add_argument(
        "--pid", type=int, help="server PID; otherwise detected with lsof"
    )
    parser.add_argument(
        "--max-concurrency",
        type=int,
        default=512,
        help="maximum simultaneous clients (1–4096)",
    )
    parser.add_argument(
        "--step",
        type=int,
        default=32,
        help="increase concurrency by this amount per stage",
    )
    parser.add_argument(
        "--stage-duration",
        type=float,
        default=10,
        help="seconds to sustain each concurrency level",
    )
    parser.add_argument(
        "--timeout", type=float, default=5, help="request timeout seconds"
    )
    parser.add_argument(
        "--connection-mode",
        choices=("keep-alive", "close"),
        default="keep-alive",
        help="reuse HTTP/1.1 connections (default) or reconnect each request",
    )
    parser.add_argument(
        "--stop-on-saturation",
        action="store_true",
        help="stop ramp on first detected saturation",
    )
    args = parser.parse_args()
    if not 1 <= args.max_concurrency <= 4096:
        parser.error("--max-concurrency must be between 1 and 4096")
    if not 1 <= args.step <= args.max_concurrency:
        parser.error("--step must be between 1 and --max-concurrency")
    if not 1 <= args.stage_duration <= 600:
        parser.error("--stage-duration must be between 1 and 600 seconds")
    if not 0.1 <= args.timeout <= 120:
        parser.error("--timeout must be between 0.1 and 120 seconds")

    parsed = urlsplit(args.url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        parser.error("--url must be an absolute http:// or https:// origin")
    if parsed.path not in {"", "/"} or parsed.query or parsed.fragment:
        parser.error("--url must contain only the origin; use --path for a URL path")

    authenticated = args.path == "/api/me"
    if authenticated and not args.phone:
        parser.error("--phone is required when --path is /api/me")
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    try:
        pid = resolve_pid(args.pid, port)
        tokens: list[str] = []
        row_ids: list[int] = []
        db: sqlite3.Connection | None = None
        if authenticated:
            db = connect_db(database_path(args.database))
            tokens, row_ids = prepare_tokens(db, args.phone, args.max_concurrency)
        try:
            return asyncio.run(run_benchmark(args, parsed, pid, tokens))
        finally:
            if db is not None:
                try:
                    revoke_tokens(db, row_ids)
                    print(f"\nRevoked {len(row_ids)} temporary benchmark session(s).")
                finally:
                    db.close()
    except KeyboardInterrupt:
        print("\nBenchmark interrupted.", file=sys.stderr)
        return 130
    except (OSError, RuntimeError, sqlite3.Error, ValueError) as error:
        print(f"Benchmark failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
