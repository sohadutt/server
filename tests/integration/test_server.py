#!/usr/bin/env python3
"""Black-box HTTP/API integration tests for a temporary server instance."""

from __future__ import annotations

import json
import os
import secrets
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path


class ServerIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.binary = Path(sys.argv[1]).resolve()
        cls.root = Path(sys.argv[2]).resolve()
        cls.temporary = tempfile.TemporaryDirectory(prefix="server-integration-")
        cls.temp_path = Path(cls.temporary.name)
        cls.admin_token = secrets.token_hex(32)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            cls.port = reservation.getsockname()[1]
        cls.database = cls.root / "build" / f"integration-users-{cls.port}.sqlite"
        cls.base_url = f"https://127.0.0.1:{cls.port}"
        cls.certificate = cls.temp_path / "server.crt"
        cls.private_key = cls.temp_path / "server.key"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                "ec_paramgen_curve:P-256", "-sha256", "-noenc", "-days", "1",
                "-subj", "/CN=127.0.0.1", "-addext",
                "subjectAltName=IP:127.0.0.1", "-keyout", os.fspath(cls.private_key),
                "-out", os.fspath(cls.certificate),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        cls.ssl_context.check_hostname = False
        cls.ssl_context.verify_mode = ssl.CERT_NONE
        config = cls.temp_path / "server.conf"
        config.write_text(
            "\n".join(
                [
                    f"port = {cls.port}",
                    "host = 127.0.0.1",
                    "worker_threads = 8",
                    "tls_enabled = true",
                    f"tls_certificate_file = {cls.certificate}",
                    f"tls_private_key_file = {cls.private_key}",
                    "log_level = info",
                    "log_console = true",
                    "log_file = false",
                    f"document_root = {cls.root / 'public'}",
                    "index_file = index.html",
                    "not_found_page = 404.html",
                    f"mime_types_file = {cls.root / 'config' / 'mime.types'}",
                    "default_mime_type = application/octet-stream",
                    "max_static_file_size_mb = 4",
                    f"database_path = {cls.database}",
                    f"admin_api_token = {cls.admin_token}",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        cls.process = subprocess.Popen(
            [os.fspath(cls.binary), os.fspath(config)],
            cwd=cls.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                output = cls.process.stdout.read() if cls.process.stdout else ""
                raise RuntimeError(
                    f"test server exited during startup ({cls.process.returncode}): {output}"
                )
            try:
                status, _, _ = cls.request("GET", "/")
                if status == 200:
                    break
            except (OSError, urllib.error.URLError):
                time.sleep(0.05)
        else:
            cls.process.terminate()
            cls.process.wait(timeout=5)
            output = cls.process.stdout.read() if cls.process.stdout else ""
            raise RuntimeError(f"test server did not start: {output}")

    @classmethod
    def tearDownClass(cls) -> None:
        process = getattr(cls, "process", None)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        temporary = getattr(cls, "temporary", None)
        if temporary is not None:
            temporary.cleanup()
        database = getattr(cls, "database", None)
        if database is not None:
            for suffix in ("", "-wal", "-shm"):
                Path(f"{database}{suffix}").unlink(missing_ok=True)

    @classmethod
    def request(
        cls,
        method: str,
        path: str,
        body: dict | None = None,
        token: str | None = None,
        admin: bool = False,
    ) -> tuple[int, dict[str, str], bytes]:
        headers: dict[str, str] = {}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        elif admin:
            headers["Authorization"] = f"Bearer {cls.admin_token}"
        request = urllib.request.Request(
            cls.base_url + path, data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(
                request, timeout=3, context=cls.ssl_context
            ) as response:
                return response.status, dict(response.headers), response.read()
        except urllib.error.HTTPError as error:
            return error.code, dict(error.headers), error.read()

    def setUp(self) -> None:
        self.phone = "+1999" + f"{secrets.randbelow(10_000_000):07d}"

    def test_static_content_and_not_found(self) -> None:
        status, headers, body = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        self.assertIn(b"HTTP Server", body)
        self.assertEqual(headers.get("X-Content-Type-Options"), "nosniff")

        status, _, body = self.request("GET", "/missing-test-page")
        self.assertEqual(status, 404)
        self.assertIn(b"Page not found", body)

    def test_signup_login_session_and_logout(self) -> None:
        status, _, body = self.request(
            "POST",
            "/api/signup",
            {"phone": self.phone, "password": "integration-password-1"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))
        signup = json.loads(body)
        token = signup["token"]
        self.assertEqual(signup["phone"], self.phone)
        self.assertEqual(signup["credits"], 0)

        status, _, body = self.request(
            "POST",
            "/api/signup",
            {"phone": self.phone, "password": "integration-password-1"},
        )
        self.assertEqual(status, 409)

        status, _, _ = self.request(
            "POST",
            "/api/login",
            {"phone": self.phone, "password": "incorrect-password"},
        )
        self.assertEqual(status, 401)

        status, _, body = self.request("GET", "/api/me", token=token)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["phone"], self.phone)

        status, _, body = self.request(
            "POST", "/api/logout", token=token
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        status, _, _ = self.request("GET", "/api/me", token=token)
        self.assertEqual(status, 401)

    def test_admin_credits_blocking_and_password_reset(self) -> None:
        status, _, body = self.request(
            "POST",
            "/api/signup",
            {"phone": self.phone, "password": "integration-password-1"},
        )
        self.assertEqual(status, 201, body.decode("utf-8", errors="replace"))
        token = json.loads(body)["token"]

        status, _, _ = self.request(
            "POST", "/api/admin/users/lookup", {"phone": self.phone}
        )
        self.assertEqual(status, 401)

        status, _, body = self.request(
            "POST",
            "/api/admin/users/credits",
            {"phone": self.phone, "credits": 25, "reference": "integration-credit"},
            admin=True,
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))

        status, _, body = self.request(
            "POST", "/api/admin/users/lookup", {"phone": self.phone}, admin=True
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["credits"], 25)

        status, _, body = self.request(
            "POST",
            "/api/admin/users/active",
            {"phone": self.phone, "active": False},
            admin=True,
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        status, _, _ = self.request("GET", "/api/me", token=token)
        self.assertEqual(status, 401)

        status, _, body = self.request(
            "POST",
            "/api/admin/users/active",
            {"phone": self.phone, "active": True},
            admin=True,
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))

        status, _, body = self.request(
            "POST",
            "/api/admin/users/password",
            {"phone": self.phone, "new_password": "integration-password-2"},
            admin=True,
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))
        status, _, _ = self.request(
            "POST",
            "/api/login",
            {"phone": self.phone, "password": "integration-password-1"},
        )
        self.assertEqual(status, 401)
        status, _, body = self.request(
            "POST",
            "/api/login",
            {"phone": self.phone, "password": "integration-password-2"},
        )
        self.assertEqual(status, 200, body.decode("utf-8", errors="replace"))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
