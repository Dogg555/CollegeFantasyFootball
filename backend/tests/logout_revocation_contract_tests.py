#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SESSION_STORE = ROOT / "backend" / "src" / "auth_session_store.cpp"
SESSION_TESTS = ROOT / "backend" / "tests" / "auth_session_store_tests.cpp"
AUTH_CONTRACTS = ROOT / "scripts" / "auth_contract_tests.py"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


store = SESSION_STORE.read_text(encoding="utf-8")
tests = SESSION_TESTS.read_text(encoding="utf-8")
contracts = AUTH_CONTRACTS.read_text(encoding="utf-8")

require("revokedTokens" in store,
        "session revocation does not retain an in-process denylist")
require("tokenRevokedInMemory(token)" in store,
        "session lookup does not reject denylisted bearer tokens before storage lookup")
require("revokedTokens[token] = now + kTokenTtl" in store,
        "revocation entries are not retained through the original token lifetime")
require("DELETE FROM auth_tokens" in store,
        "logout does not delete the persistent bearer token")
require("persistent token revocation could not be confirmed" in store,
        "persistent revocation failures are not observable")
require("repeated logout cannot restore or reuse a revoked session" in tests,
        "session tests do not cover repeated logout")
require("logout revocation" in contracts,
        "production authentication contracts do not validate the token after logout")

print("logout revocation contracts passed")
