#!/usr/bin/env python3
"""Two-user authorization, invitation, and IDOR regression tests."""
from __future__ import annotations
import json, os, time, urllib.error, urllib.parse, urllib.request

BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
PASSWORD = "Authorization-Test-2026!"

def request(path, method="GET", payload=None, token=""):
    headers = {"Accept": "application/json"}
    data = None
    if payload is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(payload).encode()
    if token: headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(BASE_URL + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as response:
            return response.status, json.loads(response.read().decode() or "{}")
    except urllib.error.HTTPError as error:
        body = error.read().decode()
        return error.code, json.loads(body or "{}")

def require(value, message):
    if not value: raise AssertionError(message)

def signup(email):
    status, body = request("/api/auth/signup", "POST", {"email": f"  {email.upper()}  ", "password": PASSWORD})
    require(status == 201, f"signup failed: {status} {body}")
    require(body.get("email") == email, f"email not canonicalized: {body}")
    return body["token"]

def denied(path, token, method="GET", payload=None):
    status, body = request(path, method, payload, token)
    require(status in {403, 404}, f"IDOR allowed: {method} {path} -> {status} {body}")

def main():
    suffix = str(int(time.time() * 1000))
    owner_email = f"authz-owner-{suffix}@example.test"
    outsider_email = f"authz-outsider-{suffix}@example.test"
    owner = signup(owner_email)
    outsider = signup(outsider_email)
    status, _ = request("/api/auth/signup", "POST", {"email": owner_email.upper(), "password": PASSWORD})
    require(status == 409, "case-variant duplicate account accepted")
    status, league = request("/api/leagues", "POST", {"name": "Authorization League", "teams": 8, "scoring": "ppr", "draftType": "snake", "invitedEmails": [], "rosterRules": {"qb":0,"rb":0,"wr":0,"te":0,"flex":0,"bench":8}}, owner)
    require(status == 201 and league.get("id"), f"league creation failed: {status} {league}")
    league_id = league["id"]
    for suffix_path in ("", "/members", "/roster", "/draft", "/transactions"):
        denied(f"/api/leagues/{league_id}{suffix_path}", outsider)
    denied(f"/api/leagues/{league_id}", outsider, "PUT", {"name":"stolen","teams":8,"scoring":"ppr","draftType":"snake"})
    denied(f"/api/leagues/{league_id}", outsider, "DELETE")
    denied(f"/api/leagues/{league_id}/join", outsider, "POST", {})
    status, body = request(f"/api/leagues/{league_id}/members", "POST", {"email": outsider_email.upper(), "role":"member"}, owner)
    require(status == 201, f"invite failed: {status} {body}")
    denied(f"/api/leagues/{league_id}", outsider)
    status, body = request(f"/api/leagues/{league_id}/join", "POST", {}, outsider)
    require(status == 202 and body.get("joinStatus") == "pending_approval", f"approval bypass: {status} {body}")
    denied(f"/api/leagues/{league_id}", outsider)
    member = urllib.parse.quote(outsider_email, safe="")
    status, body = request(f"/api/leagues/{league_id}/members/{member}", "PUT", {"role":"member","status":"Active"}, owner)
    require(status == 200, f"approval failed: {status} {body}")
    status, _ = request(f"/api/leagues/{league_id}", token=outsider)
    require(status == 200, "approved member lacks access")
    status, body = request(f"/api/leagues/{league_id}", "PUT", {
        "name": "Authorization League Updated",
        "teams": 8,
        "scoring": "ppr",
        "draftType": "snake",
        "invitedEmails": [outsider_email.upper()],
        "rosterRules": {"qb":0,"rb":0,"wr":0,"te":0,"flex":0,"bench":8},
    }, owner)
    require(status == 200, f"league settings update failed: {status} {body}")
    status, _ = request(f"/api/leagues/{league_id}", token=outsider)
    require(status == 200, "saving league settings demoted an active member")
    denied(f"/api/leagues/{league_id}", outsider, "PUT", {"name":"stolen","teams":8,"scoring":"ppr","draftType":"snake"})
    owner_path = urllib.parse.quote(owner_email, safe="")
    status, _ = request(f"/api/leagues/{league_id}/members/{owner_path}", "PUT", {"role":"member","status":"Removed"}, owner)
    require(status == 403, "league owner could be removed")
    print(json.dumps({"status":"ok","leagueId":league_id,"idorBlocked":True,"approvalRequired":True}, indent=2))

if __name__ == "__main__": main()
