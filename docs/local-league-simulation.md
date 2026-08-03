# Local league simulation

This environment creates a disposable copy of the application for automated multi-account testing. It does not use Render, production credentials, or the normal local PostgreSQL volume.

## What it starts

| Service | Default URL or port |
| --- | --- |
| Frontend | `http://127.0.0.1:13000` |
| API | `http://127.0.0.1:18080` |
| PostgreSQL | `127.0.0.1:55432` |

PostgreSQL uses an in-memory `tmpfs`. Stopping the Compose project destroys all accounts, leagues, rosters, and seeded players from the simulation.

The seed records use IDs such as `sim-player-001`. They exist only in this disposable database and are never loaded into production.

## Windows PowerShell

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-local-sim.ps1
```

Run a six-team simulation ten times:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-local-sim.ps1 `
  -Teams 6 `
  -Iterations 10 `
  -Seed 42
```

Leave the environment running so the UI can be inspected manually:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-local-sim.ps1 `
  -Teams 4 `
  -KeepEnvironment `
  -KeepData
```

Stop it later:

```powershell
docker compose -p cff-sim -f docker-compose.sim.yml down --volumes --remove-orphans
```

## macOS, Linux, or Git Bash

```bash
chmod +x scripts/run-local-sim.sh
./scripts/run-local-sim.sh --teams 4 --iterations 1
```

Six teams and ten iterations:

```bash
./scripts/run-local-sim.sh --teams 6 --iterations 10 --seed 42
```

Keep the UI available after the run:

```bash
./scripts/run-local-sim.sh --teams 4 --keep-environment --keep-data
```

## What each lifecycle tests

Each iteration performs the following actions through the real HTTP API:

1. Requires healthy API and PostgreSQL dependencies.
2. Creates four or six separate user accounts.
3. Creates a league and a join code.
4. Joins every manager and persists unique team names.
5. Verifies a normal manager cannot change commissioner settings.
6. Saves draft order and a manager-specific draft queue.
7. Sends two simultaneous requests for the same first pick and requires exactly one winner.
8. Completes the draft and verifies unique one-player rosters.
9. Accepts a trade and verifies both rosters change atomically.
10. Processes a waiver claim and verifies the add/drop result.
11. Performs a free-agent add/drop.
12. Generates, scores, and finalizes a week twice to test idempotency.
13. Verifies the transaction audit trail.
14. Signs every account in again and verifies league and roster persistence.
15. Deletes the generated league unless `--keep-data` is used.

A JSON report is written to:

```text
simulation-artifacts/league-simulation-report.json
```

## Run the simulator against an already-running API

The Python simulator can be run directly:

```powershell
python scripts/simulate_league.py `
  --base-url http://127.0.0.1:18080 `
  --teams 4 `
  --iterations 5 `
  --seed 42
```

The target database must contain at least twelve active players whose IDs start with `sim-player-`. The launcher handles this automatically.

## Useful options

| Option | Purpose |
| --- | --- |
| `--teams 4` / `-Teams 4` | Run a four-team league |
| `--teams 6` / `-Teams 6` | Run a six-team league |
| `--iterations N` / `-Iterations N` | Repeat the full lifecycle up to 100 times |
| `--seed N` / `-Seed N` | Make player selection deterministic |
| `--keep-data` / `-KeepData` | Keep the generated league for manual UI inspection |
| `--keep-environment` / `-KeepEnvironment` | Leave Docker services running |
| `--no-build` / `-NoBuild` | Reuse previously built images |
| `--skip-concurrency` / `-SkipConcurrency` | Skip the simultaneous duplicate-pick test |

## Troubleshooting

Show service status and logs:

```powershell
docker compose -p cff-sim -f docker-compose.sim.yml ps
docker compose -p cff-sim -f docker-compose.sim.yml logs backend
docker compose -p cff-sim -f docker-compose.sim.yml logs postgres
```

Reset everything:

```powershell
docker compose -p cff-sim -f docker-compose.sim.yml down --volumes --remove-orphans
docker builder prune -f
```

Change ports when the defaults are occupied:

```powershell
$env:CFF_SIM_API_PORT = '28080'
$env:CFF_SIM_FRONTEND_PORT = '23000'
$env:CFF_SIM_DB_PORT = '65432'
.\scripts\run-local-sim.ps1
```
