#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AUTH = ROOT / "frontend" / "auth.js"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require(AUTH.exists(), "frontend authentication controller is missing")
auth = AUTH.read_text(encoding="utf-8")

require("function safeNextDestination" in auth,
        "sign-in does not validate the requested post-authentication destination")
require("destination.origin !== window.location.origin" in auth,
        "post-authentication redirects allow cross-origin destinations")
require("privateDestinations.has(page)" in auth,
        "post-authentication redirects are not limited to protected application pages")
require("const requestedNext = safeNextDestination(urlParams.get('next'))" in auth,
        "the protected-page destination is not captured before URL cleanup")
require("postAuthDestination(redirectTo)" in auth,
        "successful authentication does not use the preserved destination")
require("loginStatus, requestedNext" in auth,
        "login still redirects every user to the default league page")
require("authRedirectReason === 'session-expired'" in auth,
        "the sign-in page does not explain an expired session")
require("continue where you left off" in auth,
        "expired-session messaging is not actionable")
require("Checking your saved session" in auth,
        "authentication pages show saved browser state before backend validation completes")
require("validateAuthSessionResult" in auth,
        "authentication pages cannot distinguish validation outages from invalid sessions")

print("authentication redirect contracts passed")
