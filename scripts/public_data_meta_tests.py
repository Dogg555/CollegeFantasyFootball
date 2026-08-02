#!/usr/bin/env python3
import json
import os
import urllib.request

BASE = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")


def get(path):
    request = urllib.request.Request(BASE + path, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status != 200:
            raise AssertionError(f"{path} returned {response.status}")
        return json.load(response)


players_meta = get("/api/players/meta")
assert isinstance(players_meta, dict)
assert players_meta.get("status") in {"ok", "unavailable"}
assert isinstance(players_meta.get("activePlayers", 0), int)

scores_meta = get("/api/scores/live/meta")
assert isinstance(scores_meta, dict)
assert scores_meta.get("status") in {"ok", "never", "failed", "unavailable"}
assert isinstance(scores_meta.get("gameCount", 0), int)

players = get("/api/players?limit=5&offset=0")
assert isinstance(players, list)
assert len(players) <= 5
print("public player and schedule metadata smoke tests passed")
