#!/usr/bin/env sh
set -eu

TEAMS="${CFF_SIM_TEAMS:-4}"
ITERATIONS="${CFF_SIM_ITERATIONS:-1}"
SEED="${CFF_SIM_SEED:-42}"
KEEP_ENVIRONMENT=false
KEEP_DATA=false
NO_BUILD=false
SKIP_CONCURRENCY=false

usage() {
  cat <<'EOF'
Usage: scripts/run-local-sim.sh [options]

Options:
  --teams 4|6           Number of simulated teams (default: 4)
  --iterations N        Number of complete lifecycles (default: 1)
  --seed N              Deterministic random seed (default: 42)
  --keep-environment    Leave Docker services running after the simulation
  --keep-data           Keep the generated league until the environment stops
  --no-build            Reuse existing Docker images
  --skip-concurrency    Skip the simultaneous duplicate-pick test
  -h, --help            Show this help
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --teams) TEAMS="$2"; shift 2 ;;
    --iterations) ITERATIONS="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --keep-environment) KEEP_ENVIRONMENT=true; shift ;;
    --keep-data) KEEP_DATA=true; shift ;;
    --no-build) NO_BUILD=true; shift ;;
    --skip-concurrency) SKIP_CONCURRENCY=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$TEAMS" in 4|6) ;; *) echo "--teams must be 4 or 6" >&2; exit 2 ;; esac
case "$ITERATIONS" in ''|*[!0-9]*) echo "--iterations must be a positive integer" >&2; exit 2 ;; esac
[ "$ITERATIONS" -ge 1 ] && [ "$ITERATIONS" -le 100 ] || {
  echo "--iterations must be between 1 and 100" >&2
  exit 2
}

command -v docker >/dev/null 2>&1 || {
  echo "Docker was not found. Install Docker Desktop or Docker Engine." >&2
  exit 1
}
command -v python3 >/dev/null 2>&1 || {
  echo "Python 3 was not found." >&2
  exit 1
}

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

API_PORT="${CFF_SIM_API_PORT:-18080}"
FRONTEND_PORT="${CFF_SIM_FRONTEND_PORT:-13000}"
API_URL="http://127.0.0.1:${API_PORT}"
FRONTEND_URL="http://127.0.0.1:${FRONTEND_PORT}"
COMPOSE="docker compose -p cff-sim -f docker-compose.sim.yml"

cleanup() {
  if [ "$KEEP_ENVIRONMENT" = true ]; then
    echo "Simulation environment left running."
    echo "Stop it with: docker compose -p cff-sim -f docker-compose.sim.yml down --volumes --remove-orphans"
  else
    echo "Stopping disposable simulation environment..."
    $COMPOSE down --volumes --remove-orphans >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

echo "Starting disposable CFF simulation environment..."
if [ "$NO_BUILD" = true ]; then
  $COMPOSE up -d
else
  $COMPOSE up -d --build
fi

echo "Waiting for API health at ${API_URL}/health..."
healthy=false
attempt=1
while [ "$attempt" -le 60 ]; do
  if python3 - "$API_URL" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
try:
    with urllib.request.urlopen(base + "/health", timeout=4) as response:
        payload = json.load(response)
except Exception:
    raise SystemExit(1)
raise SystemExit(0 if payload.get("status") == "ok" and payload.get("database") == "ok" else 1)
PY
  then
    healthy=true
    break
  fi
  sleep 2
  attempt=$((attempt + 1))
done

if [ "$healthy" != true ]; then
  $COMPOSE logs backend || true
  echo "The local simulation API did not become healthy." >&2
  exit 1
fi

echo "Seeding deterministic simulation players..."
$COMPOSE cp scripts/sim_seed.sql postgres:/tmp/sim_seed.sql
$COMPOSE exec -T postgres psql -U cff_sim -d cff_sim -v ON_ERROR_STOP=1 -f /tmp/sim_seed.sql

set -- scripts/simulate_league.py \
  --base-url "$API_URL" \
  --teams "$TEAMS" \
  --iterations "$ITERATIONS" \
  --seed "$SEED"
[ "$KEEP_DATA" = true ] && set -- "$@" --keep-data
[ "$SKIP_CONCURRENCY" = true ] && set -- "$@" --skip-concurrency

echo "Running ${TEAMS}-team lifecycle simulation (${ITERATIONS} iteration(s))..."
python3 "$@"

echo "Frontend: $FRONTEND_URL"
echo "API:      $API_URL"
