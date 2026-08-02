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

When `DB_URL` is set, auth and fantasy league data are persisted in Postgres. When `DB_URL` is not set, the API keeps the current local in-memory fallback for fast UI prototyping unless `CFF_REQUIRE_DB=true` is set. Use `CFF_REQUIRE_DB=true` for production and Render.

## Render deployment
The repo root includes `render.yaml` for the Docker API service, a separate static frontend service, and a managed Postgres database. The backend image includes `psql`, and Render runs:

```sh
sh /srv/db/migrate.sh
```

as a pre-deploy migration step. The runner records applied versions in `schema_migrations`, applies `schema.sql` once as `001_schema_snapshot`, then applies future files from `backend/db/migrations/*.sql`.

Runtime environment:
- `PORT` - Render sets this; default is `8080`.
- `DB_URL` - required for persistent auth, leagues, rosters, drafts, waivers, trades, scoring, and ingestion status.
- `JWT_SECRET` - required for authenticated API access.
- `ALLOWED_ORIGINS` - comma-separated frontend origins that can call the API.
- `CFBD_API_KEY` - required for CollegeFootballData ingestion.
- `CFBD_INGEST_ON_STARTUP` - keep `false` in hosted deployments; the Render cron owns full roster refreshes.
- `CFBD_INGEST_INTERVAL_HOURS` - keep `0` in hosted deployments; the Render cron owns full roster refreshes.
- `RESEND_API_KEY` - required to send password reset and email verification mail.
- `CFF_EMAIL_FROM` - verified sender address for auth emails.
- `CFF_FRONTEND_BASE_URL` - frontend origin used to build reset and verification links.
- `CFF_REQUIRE_DB` - set to `true` in production to reject auth when Postgres is not configured.
- `CFF_ALLOW_SHARED_SECRET_AUTH` - defaults off; only set `true` for legacy admin-token compatibility.
- `CFF_REQUIRE_EMAIL_VERIFICATION` - blocks login until `/api/auth/verify-email` succeeds when set to `true`.
- `CFF_EXPOSE_AUTH_TOKENS` - returns reset/verification tokens in responses for smoke tests only.
- `CFF_LOG_AUTH_TOKENS` - logs reset/verification tokens for local debugging only. Keep `false` in hosted/open-source deployments.

The Render frontend service runs `scripts/render-build-frontend.sh`, which copies `frontend/` to `frontend-dist/` and writes `frontend-dist/config.js`. Set the frontend service env var `CFF_API_BASE` to the API origin plus `/api`, for example:

```sh
CFF_API_BASE=https://YOUR-API-SERVICE.onrender.com/api
```

Set the backend `ALLOWED_ORIGINS` and `CFF_FRONTEND_BASE_URL` to the deployed frontend origin, for example `https://YOUR-FRONTEND-SERVICE.onrender.com`.

Post-deploy smoke checks:
```sh
curl https://YOUR-RENDER-SERVICE.onrender.com/health
curl https://YOUR-RENDER-SERVICE.onrender.com/api/health
curl -X POST https://YOUR-RENDER-SERVICE.onrender.com/api/auth/signup \
  -H "Content-Type: application/json" \
  -d '{"email":"admin@example.com","password":"change-me-now"}'
curl https://YOUR-RENDER-SERVICE.onrender.com/api/admin/ingest/cfbd/status \
  -H "Authorization: Bearer YOUR_TOKEN"
```

Automated smoke checks:
```sh
CFF_API_BASE_URL=https://YOUR-RENDER-SERVICE.onrender.com python scripts/api_smoke_tests.py
```

Node smoke runner for machines without Python:
```sh
CFF_API_BASE_URL=https://YOUR-RENDER-SERVICE.onrender.com node scripts/api_smoke_tests.mjs
```

## CFBD ingestion (players)
The backend can fetch player data directly from the CollegeFootballData API and persist it into Postgres.

Triggers:
- Admin HTTP: `POST /api/admin/ingest/cfbd` (requires the same bearer token used for other secure endpoints).
- Admin status: `GET /api/admin/ingest/cfbd/status`.
- Render cron: `college-ff-cfbd-ingest` runs the full roster refresh weekly.

Ingestion is intentionally not exposed in the public frontend. The Players page only browses and searches data already persisted in Postgres.

## Fantasy league API
All league and transaction routes require `Authorization: Bearer <token>`. The API enforces account ownership and a maximum of three leagues per account.

Auth:
- `POST /api/auth/signup`
- `POST /api/auth/login`
- `GET /api/auth/validate`
- `POST /api/auth/logout`
- `POST /api/auth/verify-email`
- `POST /api/auth/resend-verification`
- `POST /api/auth/request-password-reset`
- `POST /api/auth/reset-password`

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
- `POST /api/leagues/{leagueId}/waivers/{claimId}/status`
- `POST /api/leagues/{leagueId}/waivers/reorder`
- `GET /api/leagues/{leagueId}/waiver-priority`
- `POST /api/leagues/{leagueId}/waiver-priority/reset`

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
