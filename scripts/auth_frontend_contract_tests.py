#!/usr/bin/env python3
"""Static contracts for production authentication request handling."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUTH = (ROOT / "frontend" / "auth.js").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend" / "config.js").read_text(encoding="utf-8")


def require(fragment: str, message: str) -> None:
    if fragment not in AUTH:
        raise AssertionError(message)


def main() -> int:
    require("new AbortController()", "auth requests must have an abortable timeout")
    require("AUTH_REQUEST_TIMEOUT_MS", "auth timeout constant is missing")
    require("cache: 'no-store'", "auth fetches must bypass caches")
    require("credentials: 'omit'", "bearer-token auth must not send ambient cookies")
    require("dataset.submitting", "forms must prevent duplicate submissions")
    require("button.disabled = true", "submit buttons must be disabled while requests are active")
    require("Retry-After", "rate-limit retry metadata is not handled")
    require("X-CFF-Request-Id", "request correlation is not surfaced")
    require("accountMayExist", "enumeration-safe duplicate signup response is not handled")
    require("The account may already have been created", "ambiguous signup failures need recovery guidance")
    require("if (!allowLocalDemo)", "production network failures must not create local sessions")
    require("['localhost', '127.0.0.1', '::1']", "demo sessions must remain localhost-only")

    if "Account already exists" in AUTH:
        raise AssertionError("frontend must not expose registered-account existence")
    if "window.CFF_ALLOW_LOCAL_DEMO !== false" in CONFIG:
        raise AssertionError("local demo mode must fail closed")
    if "window.CFF_ALLOW_LOCAL_DEMO = window.CFF_ALLOW_LOCAL_DEMO === true" not in CONFIG:
        raise AssertionError("local demo mode must require explicit enablement")

    print("Frontend authentication contracts passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
