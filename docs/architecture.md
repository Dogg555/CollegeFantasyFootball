# College Fantasy Football Platform Architecture

This document outlines a proposed architecture for a full-stack college fantasy football application targeting Division I FBS and FCS players.

## Goals
- Provide a slick, modern experience similar to popular NFL fantasy platforms.
- Cover the full lifecycle: league creation, roster management, drafts, scoring, waivers, trades, and playoffs.
- Handle the breadth of college players (FBS + FCS) with scalable ingestion and search.
- Favor a maintainable, modular codebase with clear separation of concerns.

## System Overview
- **Frontend:** Lightweight HTML/CSS/JavaScript application served as static assets. Uses modular JavaScript for state management and fetch-based API calls. Can later adopt a component library if needed.
- **Backend:** C++17+ service providing RESTful APIs and real-time updates (SSE/WebSockets for live scoring). The service exposes endpoints for auth, league management, drafts, rosters, schedules, scoring, and user preferences.
- **Database:** PostgreSQL for transactional data (users, leagues, rosters, matchups). Redis for caching hot data (player lookups, live stats, session tokens).
- **Data ingestion:** Scheduled jobs to pull game schedules, player pools, and live stats from external feeds. Normalize and store in a canonical schema.
- **Deployment:** Containerized via Docker. CI to build/test the C++ service and lint the frontend assets.

## Key Backend Components (C++)
- **HTTP Server Framework:** Drogon (modern C++17, built-in routing, ORM support, WebSockets). Alternative: Pistache or Crow if Drogon is unavailable.
- **Modules:**
  - `auth`: signup/login, OAuth providers, session/token handling.
  - `players`: player search, details, depth charts, injury updates.
  - `leagues`: create/join leagues, scoring settings, roster rules.
  - `drafts`: snake/auction drafts, queueing, pause/resume, commissioner controls.
  - `lineups`: weekly lineup submissions and validation against roster rules.
  - `matchups`: schedule generation, head-to-head scoring, playoffs/brackets.
  - `waivers`: FAAB/priority processing, add/drop transactions.
  - `scoring`: stat mapping, live scoring aggregation, historical box scores.
  - `notifications`: email/push/web notifications for roster events and live scoring.
  - `admin`: data quality dashboards and league support tools.

## API Surface (Representative)
- `POST /api/auth/signup`, `POST /api/auth/login`
- `GET /api/players?query=` (name, team, position, conference filters)
- `GET /api/leagues` (list/joinable), `POST /api/leagues` (create)
- `GET /api/leagues/{id}`, `PATCH /api/leagues/{id}` (settings)
- `POST /api/leagues/{id}/invite`, `POST /api/leagues/{id}/join`
- `POST /api/leagues/{id}/draft/start`, `POST /api/leagues/{id}/draft/pick`
- `POST /api/leagues/{id}/lineups/submit`, `POST /api/leagues/{id}/waivers`
- `GET /api/leagues/{id}/matchups/week/{n}` (schedule + scores)
- `GET /api/scores/live` (SSE/WebSocket stream for live scoring updates)

## Data Model Notes
- **Players:** keyed by an external player ID; includes metadata (team, position, class, conference, eligibility, injury status).
- **Teams/Games:** normalized schedule table keyed by season/week/game. Supports doubleheaders and neutral-site flags.
- **Leagues:** scoring settings (PPR, yardage bonuses, defensive scoring), roster slots, waiver type, draft type, playoff weeks.
- **Rosters:** per-team roster table with slot enforcement and transaction history.
- **Matchups:** head-to-head pairings, weekly scores, playoff brackets.
- **Stats ingestion:** pipeline to transform vendor-specific stats into normalized categories; stored per game and aggregated weekly.

## Draft & Scoring Considerations
- Draft UI should support search, queueing, and autopick if a user is disconnected.
- Scoring should be configurable by league and re-calculable if rules change midseason.
- Live scoring should batch updates to limit load, with per-league throttling.
- Injury news and depth chart changes should be cached and flagged for roster validation.

## Observability & Reliability
- Structured logging with request IDs and user/league context.
- Metrics (latency, throughput, error rates, draft/waiver throughput) via Prometheus/OpenTelemetry exporters.
- Health checks for dependencies (DB, cache, message bus for ingestion).
- Rate limiting and input validation at the edge.

## Security
- HTTPS everywhere; signed JWTs or opaque tokens stored server-side (Redis) for sessions.
- RBAC for commissioner vs. team managers vs. read-only members.
- Audit logs for commissioner actions and roster changes.

## Roadmap (high level)
1. Scaffold C++ backend with Drogon, auth skeleton, and health check endpoint.
2. Implement player search and league creation APIs backed by PostgreSQL.
3. Build draft engine (snake) with WebSocket updates; add lineup submission.
4. Add waiver processing and weekly matchup scoring.
5. Integrate live scoring ingestion and notification channels.
6. Harden for scale: caching, rate limiting, background jobs, and observability.

## Local Development
- Build the backend via CMake (example provided in `backend/README.md`).
- Serve the frontend from `frontend/` during development with a simple static server.
- Use Docker Compose to run PostgreSQL and Redis locally (future addition).

## Open Questions
- Preferred data vendor(s) for comprehensive FBS/FCS stats.
- Depth chart/injury feed availability and latency requirements.
- Whether to support IDP (individual defensive players) and special teams scoring at launch.
