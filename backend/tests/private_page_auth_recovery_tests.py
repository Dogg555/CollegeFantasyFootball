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

recover_call = "await root.CFFAuthSessionSync?.recover()"
auth_read = "const auth = typeof root.getAuthState === 'function' ? root.getAuthState() : null;"
redirect = "redirectToSignIn();"

require(recover_call in alpha, "private pages do not recover the authenticated session before guarding access")
require(auth_read in alpha, "private-page guard no longer reads the shared auth state")
require(alpha.index(recover_call) < alpha.index(auth_read),
        "private-page guard reads auth before cross-tab session recovery completes")
require(alpha.index(auth_read) < alpha.index(redirect, alpha.index(auth_read)),
        "private-page guard redirect order is not explicit")
require("new window.BroadcastChannel(CHANNEL_NAME)" in sync,
        "session recovery no longer uses the cross-tab channel")
require("window.sessionStorage.setItem(AUTH_KEY" in sync,
        "recovered authentication is not written into the current tab")

print("private page authentication recovery contracts passed")
