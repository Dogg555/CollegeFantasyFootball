#!/usr/bin/env python3
"""Minimal authenticated SMTP catcher for local and CI contract tests."""

from __future__ import annotations

import argparse
import json
import socketserver
import sys
import threading
from datetime import datetime, timezone
from pathlib import Path


class SmtpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], output: Path):
        super().__init__(address, SmtpHandler)
        self.output = output
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()

    def trace(self, direction: str, line: str) -> None:
        safe_line = line
        upper = line.upper()
        if upper.startswith("AUTH ") or line_stage_value(line):
            safe_line = "<authentication exchange redacted>"
        print(f"[smtp] {direction} {safe_line}", file=sys.stderr, flush=True)

    def record(self, envelope: dict[str, object]) -> None:
        payload = {
            "receivedAt": datetime.now(timezone.utc).isoformat(),
            **envelope,
        }
        with self.lock:
            with self.output.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(payload, sort_keys=True) + "\n")
                handle.flush()


def line_stage_value(line: str) -> bool:
    """Avoid logging base64 credential challenge responses."""
    if not line or " " in line or ":" in line or "@" in line:
        return False
    alphabet = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")
    return len(line) >= 8 and all(char in alphabet for char in line)


class SmtpHandler(socketserver.StreamRequestHandler):
    server: SmtpServer

    def send(self, line: str) -> None:
        self.server.trace("S:", line)
        self.wfile.write(line.encode("ascii") + b"\r\n")
        self.wfile.flush()

    def handle(self) -> None:
        envelope: dict[str, object] = {"mailFrom": "", "recipients": [], "data": ""}
        auth_stage = ""
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[smtp] connection from {peer}", file=sys.stderr, flush=True)
        self.send("220 cff-test-smtp ESMTP ready")

        while True:
            raw = self.rfile.readline()
            if not raw:
                print(f"[smtp] connection closed by {peer}", file=sys.stderr, flush=True)
                return
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            self.server.trace("C:", line)
            upper = line.upper()

            if auth_stage == "plain":
                auth_stage = ""
                self.send("235 2.7.0 Authentication successful")
                continue
            if auth_stage == "login-username":
                auth_stage = "login-password"
                self.send("334 UGFzc3dvcmQ6")
                continue
            if auth_stage == "login-password":
                auth_stage = ""
                self.send("235 2.7.0 Authentication successful")
                continue

            if upper.startswith("EHLO"):
                self.wfile.write(
                    b"250-cff-test-smtp\r\n"
                    b"250-AUTH PLAIN LOGIN\r\n"
                    b"250 SIZE 10485760\r\n"
                )
                self.wfile.flush()
                print("[smtp] S: 250 capabilities", file=sys.stderr, flush=True)
            elif upper.startswith("HELO"):
                self.send("250 cff-test-smtp")
            elif upper == "AUTH PLAIN":
                # libcurl commonly sends AUTH PLAIN first, waits for an empty
                # challenge, and then sends the base64 credential payload.
                auth_stage = "plain"
                self.send("334")
            elif upper.startswith("AUTH PLAIN "):
                self.send("235 2.7.0 Authentication successful")
            elif upper == "AUTH LOGIN":
                auth_stage = "login-username"
                self.send("334 VXNlcm5hbWU6")
            elif upper.startswith("AUTH LOGIN "):
                auth_stage = "login-password"
                self.send("334 UGFzc3dvcmQ6")
            elif upper.startswith("MAIL FROM:"):
                envelope = {"mailFrom": line[10:].strip(), "recipients": [], "data": ""}
                self.send("250 2.1.0 Sender accepted")
            elif upper.startswith("RCPT TO:"):
                recipients = envelope.setdefault("recipients", [])
                if isinstance(recipients, list):
                    recipients.append(line[8:].strip())
                self.send("250 2.1.5 Recipient accepted")
            elif upper == "DATA":
                self.send("354 End data with <CR><LF>.<CR><LF>")
                data_lines: list[bytes] = []
                while True:
                    data_line = self.rfile.readline()
                    if not data_line or data_line in {b".\r\n", b".\n"}:
                        break
                    if data_line.startswith(b".."):
                        data_line = data_line[1:]
                    data_lines.append(data_line)
                envelope["data"] = b"".join(data_lines).decode("utf-8", errors="replace")
                self.server.record(envelope)
                self.send("250 2.0.0 Message accepted")
            elif upper == "RSET":
                envelope = {"mailFrom": "", "recipients": [], "data": ""}
                self.send("250 2.0.0 Reset")
            elif upper == "NOOP":
                self.send("250 2.0.0 OK")
            elif upper == "QUIT":
                self.send("221 2.0.0 Bye")
                return
            else:
                self.send("502 5.5.2 Command not implemented")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1025)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with SmtpServer((args.host, args.port), args.output) as server:
        print(f"SMTP test server listening on {args.host}:{args.port}", flush=True)
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
