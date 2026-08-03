#!/usr/bin/env python3
"""Move secure ping and ingestion administration routes out of main.cpp."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_path = root / "backend/src/main.cpp"
cmake_path = root / "backend/CMakeLists.txt"
contracts_path = root / "scripts/auth_contract_tests.py"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


def remove_between(text: str, start: str, end: str, label: str) -> str:
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{label}: end marker not found")
    return text[:start_index] + text[end_index:]


main = main_path.read_text(encoding="utf-8")
main = replace_once(
    main,
    '#include "health_routes.h"\n',
    '#include "health_routes.h"\n#include "operations_routes.h"\n',
    "include operations routes",
)

main = remove_between(
    main,
    "#ifdef CFF_HAS_POSTGRES\nstruct PgConnDeleter {",
    "#endif\n\nstd::string firstHeaderValue",
    "remove ingestion database helpers",
)

main = remove_between(
    main,
    '    app.registerHandler("/api/secure/ping",',
    "    cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);",
    "remove secure ping route",
)

admin_start = main.find('    app.registerHandler("/api/admin/ingest/cfbd",')
league_marker = '        .registerHandler("/api/leagues",'
admin_end = main.find(league_marker, admin_start)
if admin_start < 0 or admin_end < 0:
    raise RuntimeError("remove administration routes: source anchors not found")
main = (
    main[:admin_start]
    + "    cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);\n\n"
    + '    app.registerHandler("/api/leagues",'
    + main[admin_end + len(league_marker):]
)

for line in (
    '        .registerHandler("/api/secure/ping", preflightHandler, {drogon::Options})\n',
    '        .registerHandler("/api/admin/ingest/cfbd", preflightHandler, {drogon::Options})\n',
    '        .registerHandler("/api/admin/ingest/cfbd/status", preflightHandler, {drogon::Options})\n',
    '        .registerHandler("/api/admin/ingest/cfbd/live", preflightHandler, {drogon::Options})\n',
    '        .registerHandler("/api/admin/ingest/cfbd/live/status", preflightHandler, {drogon::Options})\n',
):
    main = replace_once(main, line, "", f"remove operations preflight {line.strip()}")

for leaked in (
    '"/api/secure/ping"',
    '"/api/admin/ingest/cfbd"',
    '"/api/admin/ingest/cfbd/status"',
    '"/api/admin/ingest/cfbd/live"',
    '"/api/admin/ingest/cfbd/live/status"',
    "Json::Value dbIngestionStatus()",
    "struct PgConnDeleter",
):
    if leaked in main:
        raise RuntimeError(f"main.cpp still contains moved operations symbol: {leaked}")

main_path.write_text(main, encoding="utf-8")

cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/health_routes.cpp\n",
    "    src/health_routes.cpp\n    src/operations_routes.cpp\n",
    "add operations route production source",
)
cmake_path.write_text(cmake, encoding="utf-8")

contracts = contracts_path.read_text(encoding="utf-8")
contracts = replace_once(
    contracts,
    'NEW_PASSWORD = os.getenv("CFF_CONTRACT_NEW_PASSWORD", "Contract-Test-New-Password-2026!")\n',
    'NEW_PASSWORD = os.getenv("CFF_CONTRACT_NEW_PASSWORD", "Contract-Test-New-Password-2026!")\nADMIN_TOKEN = os.getenv("CFF_TEST_ADMIN_TOKEN", "")\n',
    "read operations token",
)

operations_contracts = '''
    ping_without_token = call("GET", "/api/secure/ping")
    require(
        ping_without_token.status == 401 and ping_without_token.body == b"unauthorized",
        f"secure ping unauthorized contract changed: {ping_without_token.status} {ping_without_token.body!r}",
    )
    ping = expect(call("GET", "/api/secure/ping", token=session), 200, "secure ping")
    require(
        ping == {"status": "ok", "scope": "secure"},
        f"secure ping payload changed: {ping!r}",
    )

    require(ADMIN_TOKEN, "operations contract token was not provided")
    admin_missing = expect(call("GET", "/api/admin/ingest/cfbd/status"), 401, "admin status without token")
    require(admin_missing.get("error") == "Unauthorized", f"admin missing-token response changed: {admin_missing!r}")
    admin_forbidden = expect(
        call("GET", "/api/admin/ingest/cfbd/status", token=session),
        403,
        "admin status with account token",
    )
    require(
        admin_forbidden.get("error") == "Admin access required",
        f"admin account guard changed: {admin_forbidden!r}",
    )
    expect(
        call("POST", "/api/admin/ingest/cfbd", token=session),
        403,
        "manual roster ingest guard",
    )
    expect(
        call("POST", "/api/admin/ingest/cfbd/live", token=session),
        403,
        "manual live ingest guard",
    )

    ingestion_status = expect(
        call("GET", "/api/admin/ingest/cfbd/status", token=ADMIN_TOKEN),
        200,
        "administrator ingestion status",
    )
    require(ingestion_status.get("configured") is True, f"ingestion database state wrong: {ingestion_status!r}")
    require(ingestion_status.get("status") == "ok", f"ingestion status wrong: {ingestion_status!r}")
    require(ingestion_status.get("fullRosterSchedule") == "weekly", f"roster schedule changed: {ingestion_status!r}")
    require(ingestion_status.get("manualTriggerAvailable") is True, f"manual trigger flag changed: {ingestion_status!r}")
    require(isinstance(ingestion_status.get("counts"), dict), f"ingestion counts missing: {ingestion_status!r}")
    require(isinstance(ingestion_status.get("runs"), list), f"ingestion runs missing: {ingestion_status!r}")

    live_status = expect(
        call("GET", "/api/admin/ingest/cfbd/live/status", token=ADMIN_TOKEN),
        200,
        "administrator live-ingest status",
    )
    require(isinstance(live_status, dict), f"live-ingest status is not an object: {live_status!r}")

    for operations_path in (
        "/api/secure/ping",
        "/api/admin/ingest/cfbd",
        "/api/admin/ingest/cfbd/status",
        "/api/admin/ingest/cfbd/live",
        "/api/admin/ingest/cfbd/live/status",
    ):
        operations_preflight = call(
            "OPTIONS",
            operations_path,
            headers={
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "content-type, authorization",
            },
        )
        require(
            operations_preflight.status == 204,
            f"operations preflight failed for {operations_path}: {operations_preflight.status}",
        )
        require(
            operations_preflight.headers.get("access-control-allow-origin") == ORIGIN,
            f"operations CORS origin wrong for {operations_path}: {operations_preflight.headers!r}",
        )
'''
contracts = replace_once(
    contracts,
    '    validated = expect(call("GET", "/api/auth/validate", token=session), 200, "validate")\n    require(validated.get("valid") is True and validated.get("email") == EMAIL, f"validation wrong: {validated!r}")\n',
    '    validated = expect(call("GET", "/api/auth/validate", token=session), 200, "validate")\n    require(validated.get("valid") is True and validated.get("email") == EMAIL, f"validation wrong: {validated!r}")\n'
    + operations_contracts,
    "add operations runtime contracts",
)
contracts = replace_once(
    contracts,
    '        "requestLimits": True,\n',
    '        "requestLimits": True,\n        "operationsRoutes": True,\n        "operationsAuthorization": True,\n        "operationsCors": True,\n',
    "report operations contract coverage",
)
contracts_path.write_text(contracts, encoding="utf-8")

print("operations route extraction applied")
