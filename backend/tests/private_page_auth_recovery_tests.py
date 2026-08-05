#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ALPHA_UI = ROOT / "frontend" / "alpha-ui.js"
AUTH_SYNC = ROOT / "frontend" / "auth-session-sync.js"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require(ALPHA_UI.exists(), "private-page guard is missing")
require(AUTH_SYNC.exists(), "cross-tab auth recovery is missing")

alpha = ALPHA_UI.read_text(encoding="utf-8")
sync = AUTH_SYNC.read_text(encoding="utf-8")

recover_helper = "function recoverPrivatePageSession()"
wrapped_validator = "root.validateAuthSession = async function validateAuthAfterPrivatePageRecovery()"
await_recovery = "await recoverPrivatePageSession();"
auth_read = "const auth = typeof root.getAuthState === 'function' ? root.getAuthState() : null;"

require(recover_helper in alpha, "private pages do not share one authentication recovery promise")
require(wrapped_validator in alpha,
        "page initialization validation is not blocked on the private-page guard")
require("return privateGuard;" in alpha,
        "page initialization does not wait for the authoritative private-page guard")
require("root.CFFPrivatePageGuard = privateGuard" in alpha,
        "the resolved private-page guard is not exposed to page integrations")
require(alpha.index(await_recovery, alpha.index("async function guardPrivatePage"))
        < alpha.index("while (true)", alpha.index("async function guardPrivatePage")),
        "private-page guard validates before cross-tab recovery completes")
require(auth_read in alpha,
        "private-page validation does not inspect the recovered authentication state")

require("originalValidateAuthSessionResult" in alpha,
        "private-page guard cannot distinguish expired sessions from API outages")
require("if (result.unavailable)" in alpha,
        "authentication outages are not handled separately from expired sessions")
require("await waitForRetry();" in alpha,
        "authentication outages do not expose a retry path")
require("Your saved session has not been cleared" in alpha,
        "outage recovery does not explain that the saved session was preserved")
require("result.expired ? 'session-expired' : 'signin-required'" in alpha,
        "expired and missing sessions do not produce distinct sign-in reasons")
require("document.documentElement.classList.remove('cff-private-pending')" in alpha,
        "private content is not released after successful backend validation")

require("pending.responses.push(message.auth)" in sync,
        "session recovery still accepts the first response immediately")
require("function selectRecoveredAuth" in sync,
        "session recovery does not reconcile multiple responses")
require("if (accounts.size !== 1) return null" in sync,
        "conflicting account responses do not fail closed")
require("canonicalEmail(auth.email)" in sync,
        "same-account responses are not grouped by normalized email")
require("expectedEmail" in sync,
        "session recovery cannot select an explicitly expected account")
require("window.sessionStorage.setItem(AUTH_KEY" in sync,
        "recovered authentication is not written into the current tab")

print("private page authentication recovery contracts passed")
