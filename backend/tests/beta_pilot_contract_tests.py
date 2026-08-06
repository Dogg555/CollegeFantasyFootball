#!/usr/bin/env python3
"""Source contracts for the deterministic exact-commit beta pilot gate."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PILOT = (ROOT / "scripts" / "beta_pilot_runtime_contract.py").read_text(encoding="utf-8")
WORKFLOW = (ROOT / ".github" / "workflows" / "beta-pilot-lifecycle.yml").read_text(encoding="utf-8")


def require(source: str, fragment: str, message: str) -> None:
    if fragment not in source:
        raise AssertionError(message)


for endpoint in (
    '"/api/auth/signup"',
    '"/api/auth/login"',
    '"/api/auth/logout"',
    '"/api/leagues"',
    '/draft/readiness',
    '/draft/order',
    '/draft/start',
    '/draft/picks',
    '/roster/state',
    '/trades/state',
    '/trades/transactions',
    '/waivers/state',
    '/waivers/transactions',
    '/scoring/state?season=',
    '/scoring/transactions',
    '/standings?season=',
    '/transactions',
):
    require(PILOT, endpoint, f"pilot is missing lifecycle endpoint {endpoint}")

for requirement in (
    'operation_key=f"pilot-create-',
    '"expectedVersion": int(initial_draft.get("version", 0))',
    '"expectedPick": int(draft["currentPick"])',
    'offerPlayers=offer_players',
    'requestPlayers=request_players',
    'dropPlayerId=str(drop_player.get("id", ""))',
    'points == [4.0, 8.0, 12.0, 16.0]',
    'restored_standings == standings',
    'database_invariants(league_id, expected_players=8)',
):
    require(PILOT, requirement, f"pilot is missing required assertion: {requirement}")

require(PILOT, 'len(draft.get("picks", [])) == 8', "pilot must complete an eight-pick draft")
require(PILOT, '"tradePlayersPerSide": 2', "pilot must execute a two-for-two trade")
require(PILOT, 'validation.status in (401, 403)', "pilot must verify logout token invalidation")
require(PILOT, 'len({active[email] for email in emails}) == 4', "pilot must verify unique manager team names")
require(PILOT, 'call("GET", f"/api/leagues/{isolation_id}"', "pilot must test cross-league isolation")
require(PILOT, 'INSERT INTO player_stats', "pilot must seed deterministic scoring inputs")
require(PILOT, 'COUNT(DISTINCT player_id)', "pilot must reject duplicate ownership")
require(PILOT, '"status": "passed"', "pilot must emit a machine-readable passing report")

require(WORKFLOW, "name: Exact-commit beta pilot lifecycle", "workflow name is missing")
require(WORKFLOW, "services:\n      postgres:", "workflow must use isolated PostgreSQL")
require(WORKFLOW, "docker build -f backend/Dockerfile", "workflow must build the production API image")
require(WORKFLOW, "/srv/db/migrate.sh", "workflow must apply production migrations")
require(WORKFLOW, "python3 scripts/beta_pilot_runtime_contract.py", "workflow must execute the pilot")
require(WORKFLOW, "python3 backend/tests/beta_pilot_contract_tests.py", "workflow must execute source contracts")
require(WORKFLOW, "git clone --depth 1", "workflow must check out the exact branch without marketplace actions")
require(WORKFLOW, "CFF_SECURITY_ENFORCE_PRODUCTION=true", "pilot API must use production security enforcement")
require(WORKFLOW, "CFF_REQUIRE_EMAIL_VERIFICATION=false", "disposable contract accounts must not depend on external email")
require(WORKFLOW, "CFF_EXPOSE_AUTH_TOKENS=false", "production token exposure mode must remain disabled")
require(WORKFLOW, "CFF_BETA_PILOT_REPORT: /tmp/beta-pilot-runtime.json", "workflow must retain a structured report")
if "uses:" in WORKFLOW:
    raise AssertionError("beta pilot workflow must not depend on marketplace actions")

print("beta pilot source contracts passed")
