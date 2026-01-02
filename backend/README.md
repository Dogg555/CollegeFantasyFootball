# Backend snapshot

This folder holds the server scaffold for the project. It is intentionally minimal and meant for local exploration.

## Contents
- `src/` — HTTP entry point, in-memory auth mock, and placeholder domain handlers.
- `db_schema/` (gitignored) — local Postgres schema snapshots for teams, players, games, and category-aware player stats.
- `Dockerfile` and `CMakeLists.txt` — build and container hints for local runs.

## Safety reminders
- Keep `.env`, `.env.local`, TLS keys, and database credentials out of version control.
- Avoid committing database dumps or generated binaries; prefer a clean build each time.
- Remove any stray certificates or secret keys from the repo history if they appear.

## Quick build hint
With Drogon and dependencies available, configure and build using CMake into `build/`, or run through the provided Dockerfile for an isolated test run. Configure environment variables like `JWT_SECRET`, `DB_URL`, and `ALLOWED_ORIGINS` at runtime rather than hardcoding them.

## CFBD ingestion (players)
The backend can fetch player data directly from the CollegeFootballData API and persist it into Postgres.

Required env vars:
- `CFBD_API_KEY`: CFBD bearer token.
- `DB_URL`: Postgres connection string (use `postgres://...` and the `postgres` service name when running in Docker compose).

Optional env vars:
- `CFBD_BASE_URL`: Override API host (defaults to `https://api.collegefootballdata.com`).
- `CFBD_SEASON`: Season year to request (defaults to the current year).
- `CFBD_MAX_PAGES`: Limit pagination calls to stay under API quotas (defaults to 200; lower for stricter budgets).
- `CFBD_INGEST_ON_STARTUP`: Set to `true` to run ingestion when the server boots.

Triggers:
- Admin HTTP: `POST /api/admin/ingest/cfbd` (requires the same bearer token used for other secure endpoints).

Verification examples:
- psql counts: `psql "$DB_URL" -c "SELECT COUNT(*) FROM players;"` and `psql "$DB_URL" -c "SELECT id, full_name, position, conference FROM players LIMIT 5;"`.
- Player search: `curl "http://localhost:8080/api/players?query=smith&limit=5"`.
