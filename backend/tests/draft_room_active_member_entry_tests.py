#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "frontend/draft-auth-bridge.js"
DRAFT = ROOT / "frontend/draft.js"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require(BRIDGE.exists(), "draft access bridge is missing")
require(DRAFT.exists(), "draft room client is missing")

bridge = BRIDGE.read_text(encoding="utf-8")
draft = DRAFT.read_text(encoding="utf-8")

require("window.canEnterDraftRoom = function canActiveMemberEnterDraftRoom" in bridge,
        "draft access is not overridden with an active-member gate")
require("canonicalEmail(member.email) === accountEmail" in bridge,
        "draft access does not match the signed-in account to a league member")
require("String(member.status || '').toLowerCase() === 'active'" in bridge,
        "draft access does not require confirmed active membership")
require("league.draftLobbyOpen" not in bridge,
        "active managers are still blocked until the commissioner opens the lobby")
require("window.addEventListener('load', installDraftRoomAccessGate" in bridge,
        "draft access override is not installed after the draft client loads")
require("window.renderAll()" in bridge,
        "draft room does not rerender after installing the active-member gate")
require("await refreshDraftFromApi();" in draft,
        "draft room does not refresh authoritative state for members who enter early")
require("startDraftSyncPolling" in draft and "2000" in draft,
        "draft room does not keep all entered managers synchronized")

print("active league member draft room entry contracts passed")
