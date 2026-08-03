#!/usr/bin/env python3
"""Runtime contracts for public player-catalog and live-score endpoints."""
from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any

BASE = os.getenv("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ORIGIN = os.getenv("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")


class ContractFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> Any:
        try:
            return json.loads(self.body.decode()) if self.body else None
        except json.JSONDecodeError as exc:
            raise ContractFailure(f"HTTP {self.status} returned invalid JSON: {self.body[:300]!r}") from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def call(method: str, path: str, *, headers: dict[str, str] | None = None) -> Response:
    request_headers = {"Accept": "application/json", "Origin": ORIGIN, **(headers or {})}
    request = urllib.request.Request(BASE + path, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return Response(
                response.status,
                {key.lower(): value for key, value in response.headers.items()},
                response.read(),
            )
    except urllib.error.HTTPError as error:
        return Response(
            error.code,
            {key.lower(): value for key, value in error.headers.items()},
            error.read(),
        )
    except urllib.error.URLError as error:
        raise ContractFailure(f"{method} {path} could not reach {BASE}: {error}") from error


def expect_json(path: str, expected_status: int = 200) -> tuple[Any, Response]:
    response = call("GET", path)
    body = response.json()
    require(
        response.status == expected_status,
        f"GET {path}: expected {expected_status}, got {response.status}: {body!r}",
    )
    require(
        response.headers.get("access-control-allow-origin") == ORIGIN,
        f"GET {path}: CORS origin changed: {response.headers!r}",
    )
    return body, response


def wait_for_api() -> None:
    last = "no response"
    for _ in range(90):
        try:
            response = call("GET", "/api/players/meta")
            if response.status == 200:
                return
            last = f"HTTP {response.status}"
        except Exception as exc:
            last = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become ready: {last}")


def player_ids(path: str) -> list[str]:
    body, _ = expect_json(path)
    require(isinstance(body, list), f"{path}: player search must return an array: {body!r}")
    ids = [str(item.get("id", "")) for item in body]
    require(all(ids), f"{path}: player id missing: {body!r}")
    return ids


def main() -> int:
    wait_for_api()

    scores, _ = expect_json("/api/scores/live")
    require(isinstance(scores, list), f"live scores must be an array: {scores!r}")
    require(len(scores) == 2, f"seeded live score count changed: {scores!r}")
    require({game.get("id") for game in scores} == {"public-game-1", "public-game-2"}, f"live score ids changed: {scores!r}")

    score_meta, _ = expect_json("/api/scores/live/meta")
    require(isinstance(score_meta, dict), f"live score meta must be an object: {score_meta!r}")
    require(score_meta.get("databaseConfigured") is True, f"live score database state changed: {score_meta!r}")
    require(score_meta.get("status") == "ok", f"live score status changed: {score_meta!r}")
    require(score_meta.get("gameCount") == 2, f"live score count changed: {score_meta!r}")
    require(score_meta.get("liveGameCount") == 1, f"live game count changed: {score_meta!r}")

    player_meta, _ = expect_json("/api/players/meta")
    require(isinstance(player_meta, dict), f"player meta must be an object: {player_meta!r}")
    require(player_meta.get("databaseConfigured") is True, f"player database state changed: {player_meta!r}")
    require(player_meta.get("status") == "ok", f"player meta status changed: {player_meta!r}")
    require(int(player_meta.get("activePlayers", 0)) >= 105, f"seeded players missing: {player_meta!r}")
    require(int(player_meta.get("season", 0)) == 2026, f"player season changed: {player_meta!r}")

    capped = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "limit": 500}))
    require(len(capped) == 100, f"player limit cap changed: {len(capped)}")

    defaulted = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "limit": 0}))
    require(len(defaulted) == 25, f"zero-limit default changed: {len(defaulted)}")

    invalid_limit = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "limit": "invalid"}))
    require(len(invalid_limit) == 25, f"invalid-limit default changed: {len(invalid_limit)}")

    first_page = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "limit": 3, "offset": 0}))
    second_page = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "limit": 3, "offset": 3}))
    require(len(first_page) == 3 and len(second_page) == 3, "player pagination size changed")
    require(set(first_page).isdisjoint(second_page), f"player pagination overlaps: {first_page!r} {second_page!r}")

    quarterbacks = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "position": "QB", "limit": 100}))
    require(quarterbacks, "position filter returned no seeded quarterbacks")
    require(all(int(player_id.rsplit("-", 1)[-1]) % 5 == 1 for player_id in quarterbacks), f"position filtering changed: {quarterbacks!r}")

    sec_players = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "conference": "SEC", "limit": 100}))
    require(sec_players, "conference filter returned no seeded players")
    require(all(int(player_id.rsplit("-", 1)[-1]) % 3 == 0 for player_id in sec_players), f"conference filtering changed: {sec_players!r}")

    wyoming_players = player_ids("/api/players?" + urllib.parse.urlencode({"query": "Public Contract", "team": "Wyoming", "limit": 100}))
    require(wyoming_players, "team filter returned no seeded players")
    require(all(int(player_id.rsplit("-", 1)[-1]) % 2 == 0 for player_id in wyoming_players), f"team filtering changed: {wyoming_players!r}")

    for path in (
        "/api/scores/live",
        "/api/scores/live/meta",
        "/api/players",
        "/api/players/meta",
    ):
        preflight = call(
            "OPTIONS",
            path,
            headers={
                "Access-Control-Request-Method": "GET",
                "Access-Control-Request-Headers": "content-type, authorization",
            },
        )
        require(preflight.status == 204, f"OPTIONS {path}: expected 204, got {preflight.status}")
        require(
            preflight.headers.get("access-control-allow-origin") == ORIGIN,
            f"OPTIONS {path}: CORS origin changed: {preflight.headers!r}",
        )
        require("get" in preflight.headers.get("access-control-allow-methods", "").lower(), f"OPTIONS {path}: GET absent from CORS")

    print(
        json.dumps(
            {
                "status": "ok",
                "liveScores": True,
                "scoreMeta": True,
                "playerMeta": True,
                "playerSearch": True,
                "limitDefaults": True,
                "limitCap": True,
                "pagination": True,
                "filters": True,
                "cors": True,
                "authorization": "public",
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
