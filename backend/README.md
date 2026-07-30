# Backend snapshot

This folder holds the C++ Drogon API for the project.

## Contents
- `src/` - HTTP entry point, auth, player search, CFBD ingestion, and fantasy league handlers.
- `db/schema.sql` - Postgres schema for player data, accounts, leagues, rosters, waivers, trades, and transactions.
- `Dockerfile` and `CMakeLists.txt` - build and container config for local runs and Render.

## Safety reminders
- Keep `.env`, `.env.local`, TLS keys, and database credentials out of version control.
- Avoid committing database dumps or generated binaries; prefer a clean build each time.
- Remove any stray certificates or secret keys from the repo history if they appear.

## Quick build hint
With Drogon and dependencies available, configure and build using CMake into `build/`, or run through the provided Dockerfile for an isolated test run. Configure environment variables like `JWT_SECRET`, `DB_URL`, and `ALLOWED_ORIGINS` at runtime rather than hardcoding them.

When `DB_URL` is set, auth and fantasy league data are persisted in Postgres. When `DB_URL` is not set, the API keeps the current local in-memory fallback for fast UI prototyping.

## Render deployment
The repo root includes `render.yaml` for a Docker web service and a managed Postgres database. The backend image includes `psql`, and Render runs:

```sh
psql "$DB_URL" -f /srv/db/schema.sql
```

as a pre-deploy migration step.

## CFBD ingestion (players)
The backend can fetch player data directly from the CollegeFootballData API and persist it into Postgres.

Triggers:
- Admin HTTP: `POST /api/admin/ingest/cfbd` (requires the same bearer token used for other secure endpoints).

## Fantasy league API
All league and transaction routes require `Authorization: Bearer <token>`. The API enforces account ownership and a maximum of three leagues per account.

League management:
- `GET /api/leagues`
- `POST /api/leagues`
- `GET /api/leagues/{leagueId}`
- `PUT /api/leagues/{leagueId}`
- `DELETE /api/leagues/{leagueId}`

Members and invites:
- `GET /api/leagues/{leagueId}/members`
- `POST /api/leagues/{leagueId}/members`
- `PUT /api/leagues/{leagueId}/members/{memberEmail}`
- `POST /api/leagues/{leagueId}/join`

Roster and free agency:
- `GET /api/leagues/{leagueId}/roster`
- `GET /api/leagues/{leagueId}/rosters/{managerEmail}`
- `POST /api/leagues/{leagueId}/roster`
- `POST /api/leagues/{leagueId}/roster/drop`
- `POST /api/leagues/{leagueId}/roster/{playerId}/slot`
- `GET /api/leagues/{leagueId}/free-agents`

Draft:
- `GET /api/leagues/{leagueId}/draft`
- `PUT /api/leagues/{leagueId}/draft/queue`
- `POST /api/leagues/{leagueId}/draft/picks`
- `POST /api/leagues/{leagueId}/draft/reset`

Waivers:
- `GET /api/leagues/{leagueId}/waivers`
- `POST /api/leagues/{leagueId}/waivers`
- `POST /api/leagues/{leagueId}/waivers/process`
- `POST /api/leagues/{leagueId}/waivers/{claimId}/process`

Trades:
- `GET /api/leagues/{leagueId}/trades`
- `POST /api/leagues/{leagueId}/trades`
- `POST /api/leagues/{leagueId}/trades/{tradeId}/status`

Matchups:
- `GET /api/leagues/{leagueId}/matchups`
- `POST /api/leagues/{leagueId}/matchups/generate`
- `POST /api/leagues/{leagueId}/matchups/generate-season`
- `POST /api/leagues/{leagueId}/score/week/{week}`
- `POST /api/leagues/{leagueId}/score/week/{week}/finalize`

Activity:
- `GET /api/leagues/{leagueId}/transactions`
