#!/usr/bin/env python3
"""Structural contracts for public player and live-score route ownership."""
from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "backend/src/main.cpp"
HEADER = ROOT / "backend/src/public_routes.h"
SOURCE = ROOT / "backend/src/public_routes.cpp"
CMAKE = ROOT / "backend/CMakeLists.txt"

EXPECTED_PATHS = (
    "/api/scores/live",
    "/api/scores/live/meta",
    "/api/players",
    "/api/players/meta",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def registrations(source: str) -> list[tuple[str, str]]:
    pattern = re.compile(
        r"(?:app)?\.registerHandler\s*\(\s*\"([^\"]+)\".*?\{\s*drogon::(Get|Options)\s*\}\s*\)",
        re.DOTALL,
    )
    return pattern.findall(source)


def main() -> int:
    main_source = MAIN.read_text()
    header = HEADER.read_text()
    source = SOURCE.read_text()
    cmake = CMAKE.read_text()

    require('#include "public_routes.h"' in main_source, "main.cpp does not include public_routes.h")
    require(
        main_source.count("cff::public_api::registerPublicRoutes(app, allowedOrigins);") == 1,
        "main.cpp must delegate public route registration exactly once",
    )
    require(
        main_source.index("cff::public_api::registerPublicRoutes") < main_source.index("app.run();"),
        "public routes must be registered before the server starts",
    )
    for path in EXPECTED_PATHS:
        require(path not in main_source, f"main.cpp still owns {path}")

    require("namespace cff::public_api" in header, "public route namespace changed")
    require("registerPublicRoutes" in header, "public route interface missing")
    require("jwtSecret" not in header, "public routes unexpectedly depend on authentication secrets")
    require("src/public_routes.cpp" in cmake, "production target does not compile public_routes.cpp")

    found = registrations(source)
    counts = Counter(found)
    expected = Counter(
        [(path, "Get") for path in EXPECTED_PATHS]
        + [(path, "Options") for path in EXPECTED_PATHS]
    )
    require(counts == expected, f"public route registrations changed: {counts!r}")
    require(source.count("buildPreflightResponse") == 1, "public preflights must use shared CORS handling")
    require("requireAccount" not in source, "public endpoints became account-protected")
    require("requireAdmin" not in source, "public endpoints became administrator-protected")

    required_behavior = (
        "cachedLiveScorePayload()",
        "cachedLiveScoreMeta()",
        "playerCatalogMeta()",
        'getParameter("query")',
        'getOptionalParam(request, "position")',
        'getOptionalParam(request, "conference")',
        'getOptionalParam(request, "team")',
        "std::size_t limit = 25",
        "std::min<std::size_t>(parsed, 100)",
        "std::size_t offset = 0",
        "std::min<std::size_t>(parsed, 5000)",
        "cff::searchPlayers(",
        "Json::Value payload(Json::arrayValue)",
        "player.toJson()",
        "drogon::k503ServiceUnavailable",
    )
    for token in required_behavior:
        require(token in source, f"public route behavior contract missing: {token}")

    require(source.count("drogon::k200OK") == 4, "all four public GET routes must retain HTTP 200 responses")

    print(
        json.dumps(
            {
                "status": "ok",
                "businessRoutes": 4,
                "preflightRoutes": 4,
                "publicAuthorization": True,
                "queryContracts": True,
                "mainDelegation": True,
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
