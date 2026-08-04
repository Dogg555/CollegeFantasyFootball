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
original_validation = "return originalValidateAuthSession();"
auth_read = "const auth = typeof root.getAuthState === 'function' ? root.getAuthState() : null;"

require(recover_helper in alpha, "private pages do not share one authentication recovery promise")
require(wrapped_validator in alpha,
        "page initialization validation is not blocked on authentication recovery")
require(alpha.index(await_recovery, alpha.index(wrapped_validator))
        < alpha.index(original_validation, alpha.index(wrapped_validator)),
        "page initialization validates before authentication recovery completes")
require("root.CFFPrivatePageGuard = privateGuard" in alpha,
        "the resolved private-page guard is not exposed to page integrations")
require(alpha.index(await_recovery, alpha.index("async function guardPrivatePage"))
        < alpha.index(auth_read),
        "private-page guard reads auth before cross-tab recovery completes")

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
