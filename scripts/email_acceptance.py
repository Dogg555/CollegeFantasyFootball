#!/usr/bin/env python3
"""Prove verification and password-recovery delivery through a real inbox."""
from __future__ import annotations

import email
import html
import imaplib
import os
import re
import ssl
import time
import urllib.parse
from email import policy
from email.message import Message
from email.utils import parseaddr
from typing import Any

from release_gate_common import (
    GateFailure,
    JsonHttpClient,
    add_check,
    enforce_checks,
    int_env,
    mask_email,
    redact_url,
    require_env,
    utc_now,
    write_report,
    write_failure_report,
)


VERIFY_SUBJECT = "Verify your College Fantasy account"
RESET_SUBJECT = "Reset your College Fantasy password"


def plus_alias(address: str, suffix: str) -> str:
    if "@" not in address:
        raise GateFailure("CFF_EMAIL_TEST_ADDRESS must be a valid email address")
    local, domain = address.rsplit("@", 1)
    local = local.split("+", 1)[0]
    return f"{local}+{suffix}@{domain}"


def message_text(message: Message) -> str:
    parts: list[str] = []
    if message.is_multipart():
        for part in message.walk():
            if part.get_content_maintype() != "text":
                continue
            disposition = part.get_content_disposition()
            if disposition == "attachment":
                continue
            try:
                parts.append(str(part.get_content()))
            except Exception:
                payload = part.get_payload(decode=True) or b""
                parts.append(payload.decode(part.get_content_charset() or "utf-8", errors="replace"))
    else:
        try:
            parts.append(str(message.get_content()))
        except Exception:
            payload = message.get_payload(decode=True) or b""
            parts.append(payload.decode(message.get_content_charset() or "utf-8", errors="replace"))
    return html.unescape("\n".join(parts))


def extract_action_url(message: Message, required_path: str) -> str:
    body = message_text(message)
    urls = re.findall(r"https?://[^\s<>\"']+", body)
    for candidate in urls:
        cleaned = candidate.rstrip(".,);]")
        parsed = urllib.parse.urlsplit(cleaned)
        if parsed.path.endswith(required_path) and urllib.parse.parse_qs(parsed.query).get("token"):
            return cleaned
    raise GateFailure(f"Email did not contain a {required_path} link")


def token_from_url(url: str) -> str:
    values = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query).get("token", [])
    if not values or not values[0]:
        raise GateFailure("Email action link did not contain a token")
    return values[0]


def origin(value: str) -> tuple[str, str, int | None]:
    parsed = urllib.parse.urlsplit(value)
    return parsed.scheme.lower(), (parsed.hostname or "").lower(), parsed.port


def recipient_headers(message: Message) -> str:
    values = []
    for key in ("to", "delivered-to", "x-original-to", "envelope-to"):
        values.extend(message.get_all(key, []))
    return " ".join(str(value) for value in values).lower()


class Inbox:
    def __init__(self) -> None:
        self.host = require_env("CFF_IMAP_HOST")
        self.port = int_env("CFF_IMAP_PORT", 993, 1, 65535)
        self.username = os.environ.get("CFF_IMAP_USERNAME", "").strip() or require_env("CFF_EMAIL_TEST_ADDRESS")
        self.password = require_env("CFF_IMAP_PASSWORD")
        self.folder = os.environ.get("CFF_IMAP_FOLDER", "INBOX").strip() or "INBOX"
        self.connection: imaplib.IMAP4_SSL | None = None

    def __enter__(self) -> "Inbox":
        context = ssl.create_default_context()
        self.connection = imaplib.IMAP4_SSL(self.host, self.port, ssl_context=context)
        self.connection.login(self.username, self.password)
        status, _ = self.connection.select(self.folder)
        if status != "OK":
            raise GateFailure(f"Unable to select IMAP folder {self.folder}")
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if self.connection is None:
            return
        try:
            self.connection.close()
        except Exception:
            pass
        try:
            self.connection.logout()
        except Exception:
            pass

    def uid_snapshot(self) -> int:
        assert self.connection is not None
        status, rows = self.connection.uid("search", None, "ALL")
        if status != "OK" or not rows or not rows[0]:
            return 0
        return max(int(value) for value in rows[0].split())

    def wait_for(self, *, subject: str, recipient: str, after_uid: int, timeout_seconds: int) -> tuple[int, Message]:
        assert self.connection is not None
        deadline = time.monotonic() + timeout_seconds
        recipient_lower = recipient.lower()
        while time.monotonic() < deadline:
            self.connection.noop()
            self.connection.select(self.folder)
            status, rows = self.connection.uid("search", None, "UID", f"{after_uid + 1}:*")
            if status == "OK" and rows and rows[0]:
                uids = sorted((int(value) for value in rows[0].split()), reverse=True)
                for uid in uids:
                    fetch_status, payload = self.connection.uid("fetch", str(uid), "(RFC822)")
                    if fetch_status != "OK" or not payload:
                        continue
                    raw = next((item[1] for item in payload if isinstance(item, tuple) and len(item) > 1), None)
                    if not raw:
                        continue
                    message = email.message_from_bytes(raw, policy=policy.default)
                    if str(message.get("subject", "")).strip() != subject:
                        continue
                    if recipient_lower not in recipient_headers(message):
                        continue
                    return uid, message
            time.sleep(5)
        raise GateFailure(f"Timed out waiting for email subject={subject!r} recipient={recipient}")


def main() -> int:
    base_url = require_env("CFF_API_BASE_URL")
    base_address = require_env("CFF_EMAIL_TEST_ADDRESS").lower()
    frontend_base = require_env("CFF_FRONTEND_BASE_URL").rstrip("/")
    password = os.environ.get("CFF_EMAIL_ACCEPTANCE_PASSWORD", "CffEmailGate123!")
    if len(password) < 12:
        raise GateFailure("CFF_EMAIL_ACCEPTANCE_PASSWORD must be at least 12 characters")
    expected_from_domain = os.environ.get("CFF_EMAIL_EXPECTED_FROM_DOMAIN", "").strip().lower().lstrip("@")
    timeout_seconds = int_env("CFF_EMAIL_WAIT_SECONDS", 240, 30, 900)
    report_dir = os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts")
    suffix = f"cff-email-{int(time.time())}"
    account_email = plus_alias(base_address, suffix)
    nonexistent_email = plus_alias(base_address, f"cff-missing-{int(time.time())}")
    new_password = f"{password}Reset1!"
    client = JsonHttpClient(base_url)
    checks: list[dict[str, Any]] = []
    evidence: dict[str, Any] = {"account": mask_email(account_email), "messages": []}

    health = client.request("GET", "/health").payload
    add_check(checks, "Email provider configured", isinstance(health, dict) and health.get("emailDeliveryConfigured") is True, str(health))

    with Inbox() as inbox:
        before_signup = inbox.uid_snapshot()
        signup = client.request(
            "POST",
            "/api/auth/signup",
            body={"email": account_email, "password": password},
            expected=(201,),
        ).payload
        add_check(checks, "Verification required", not bool(signup.get("token")) and signup.get("emailVerificationRequired") is True, str(signup))
        add_check(checks, "Provider accepted verification email", signup.get("emailSent") is True, f"emailSent={signup.get('emailSent')}")

        verify_uid_1, verify_message_1 = inbox.wait_for(
            subject=VERIFY_SUBJECT,
            recipient=account_email,
            after_uid=before_signup,
            timeout_seconds=timeout_seconds,
        )
        verify_url_1 = extract_action_url(verify_message_1, "/verify-email.html")
        verify_token_1 = token_from_url(verify_url_1)
        sender_1 = str(verify_message_1.get("from", ""))
        add_check(checks, "Verification email delivered", True, f"uid={verify_uid_1}; from={sender_1}")
        add_check(
            checks,
            "Verification link uses configured frontend",
            origin(verify_url_1) == origin(frontend_base),
            redact_url(verify_url_1),
        )
        if expected_from_domain:
            sender_address = parseaddr(sender_1)[1].lower()
            sender_domain = sender_address.rsplit("@", 1)[1] if "@" in sender_address else ""
            add_check(checks, "Verification sender domain", sender_domain == expected_from_domain, f"senderDomain={sender_domain}")
        evidence["messages"].append({"kind": "verification-initial", "uid": verify_uid_1, "messageId": str(verify_message_1.get("message-id", "")), "from": sender_1})

        before_resend = inbox.uid_snapshot()
        resend = client.request(
            "POST",
            "/api/auth/resend-verification",
            body={"email": account_email},
        ).payload
        add_check(checks, "Resend request accepted", resend.get("status") == "ok", str(resend))
        verify_uid_2, verify_message_2 = inbox.wait_for(
            subject=VERIFY_SUBJECT,
            recipient=account_email,
            after_uid=before_resend,
            timeout_seconds=timeout_seconds,
        )
        verify_url_2 = extract_action_url(verify_message_2, "/verify-email.html")
        verify_token_2 = token_from_url(verify_url_2)
        add_check(checks, "Resent verification email delivered", verify_uid_2 > verify_uid_1, f"uids={verify_uid_1},{verify_uid_2}")
        evidence["messages"].append({"kind": "verification-resend", "uid": verify_uid_2, "messageId": str(verify_message_2.get("message-id", "")), "from": str(verify_message_2.get("from", ""))})

        client.request("POST", "/api/auth/verify-email", body={"token": verify_token_1}, expected=(400,))
        add_check(checks, "Superseded verification token rejected", True, "initial token returned HTTP 400")
        verified = client.request("POST", "/api/auth/verify-email", body={"token": verify_token_2}).payload
        add_check(checks, "Verification link activates account", verified.get("emailVerified") is True, str(verified))

        login = client.request("POST", "/api/auth/login", body={"email": account_email, "password": password}).payload
        session_token = login.get("token")
        add_check(checks, "Verified account can sign in", bool(session_token), "session token returned")

        before_reset = inbox.uid_snapshot()
        reset_request = client.request(
            "POST",
            "/api/auth/request-password-reset",
            body={"email": account_email},
        ).payload
        reset_uid, reset_message = inbox.wait_for(
            subject=RESET_SUBJECT,
            recipient=account_email,
            after_uid=before_reset,
            timeout_seconds=timeout_seconds,
        )
        reset_url = extract_action_url(reset_message, "/reset-password.html")
        reset_token = token_from_url(reset_url)
        add_check(checks, "Password-reset email delivered", True, f"uid={reset_uid}; from={reset_message.get('from', '')}")
        evidence["messages"].append({"kind": "password-reset", "uid": reset_uid, "messageId": str(reset_message.get("message-id", "")), "from": str(reset_message.get("from", ""))})

        reset = client.request(
            "POST",
            "/api/auth/reset-password",
            body={"token": reset_token, "password": new_password},
        ).payload
        add_check(checks, "Password reset completed", reset.get("status") == "ok", str(reset))
        if session_token:
            client.request("GET", "/api/auth/validate", token=session_token, expected=(401,))
            add_check(checks, "Password reset revokes sessions", True, "old session returned HTTP 401")
        client.request("POST", "/api/auth/login", body={"email": account_email, "password": password}, expected=(401,))
        add_check(checks, "Old password rejected", True, "old password returned HTTP 401")
        new_login = client.request("POST", "/api/auth/login", body={"email": account_email, "password": new_password}).payload
        add_check(checks, "New password accepted", bool(new_login.get("token")), "new session token returned")
        client.request("POST", "/api/auth/reset-password", body={"token": reset_token, "password": password}, expected=(400,))
        add_check(checks, "Reset token is single-use", True, "reused reset token returned HTTP 400")

        missing_reset = client.request(
            "POST",
            "/api/auth/request-password-reset",
            body={"email": nonexistent_email},
        ).payload
        add_check(
            checks,
            "Password-reset enumeration protection",
            missing_reset.get("message") == reset_request.get("message") and missing_reset.get("status") == reset_request.get("status"),
            f"existing={reset_request}; missing={missing_reset}",
        )

    report = {
        "title": "Transactional email acceptance",
        "generatedAt": utc_now(),
        "status": "passed" if all(check["passed"] or not check["required"] for check in checks) else "failed",
        "classification": "email-ready" if all(check["passed"] or not check["required"] for check in checks) else "email-blocked",
        "apiBaseUrl": base_url,
        "checks": checks,
        "evidence": evidence,
        "summary": "Verification delivery, resend replacement, account activation, password recovery, session revocation, single-use tokens, and enumeration-safe responses were exercised through a real IMAP inbox.",
    }
    write_report(report, report_dir, "transactional-email-acceptance")
    enforce_checks(checks)
    print("Transactional email acceptance passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        write_failure_report("Transactional email acceptance", "transactional-email-acceptance", exc)
        print(f"Transactional email acceptance failed: {exc}", file=os.sys.stderr)
        raise SystemExit(1)
