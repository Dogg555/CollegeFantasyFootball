# Backend (C++)

This directory contains the C++ backend for the College Fantasy Football platform.

## Proposed stack
- **Framework:** [Drogon](https://github.com/drogonframework/drogon) (C++17 HTTP/WebSocket/ORM)
- **Database:** PostgreSQL (with Drogon ORM) + Redis for caching/sessions
- **Build:** CMake 3.15+

If Drogon is unavailable in your environment, Crow or Pistache are viable alternatives; the structure below remains similar.

## Building (example with Drogon)
```bash
cmake -S . -B build
cmake --build build
./build/college_ff_server
```

## Dependencies
- C++17 toolchain, CMake
- Drogon (libdrogon-dev) + OpenSSL (libssl-dev) + JsonCpp (libjsoncpp-dev) + UUID (uuid-dev) + zlib
- Install on Debian/Ubuntu with the helper script at the repo root:
  ```bash
  ./scripts/install_dependencies.sh
  ```
- Windows 11 setup (vcpkg or WSL) is documented in `../docs/windows-setup.md`; automated install script: `../scripts/install_windows.ps1`.

### Docker (local testing)
```bash
docker build -t college-ff-backend .
docker run --rm -p 8080:8080 -e JWT_SECRET=dev-secret-token college-ff-backend
```

## Directory structure
- `src/main.cpp`: Entry point; wires HTTP routes and health checks.
- `src/routes/`: Route handlers grouped by domain (auth, leagues, players, drafts, waivers, lineups, scoring).
- `src/models/`: Data models and mappers (if using ORM).
- `src/services/`: Business logic (scoring rules, draft engine, waiver processing).
- `src/utils/`: Helpers (validation, logging, time, ID generation).
- `db/schema.sql`: Postgres schema for teams, players, games, stats, depth charts, and fantasy overlays.

## Environment variables (planned)
- `DB_URL` (PostgreSQL connection string)
- `REDIS_URL`
- `JWT_SECRET`
- `PORT` (defaults to 8080)
- `SSL_CERT_FILE` / `SSL_KEY_FILE` (optional HTTPS)
- `ALLOWED_ORIGINS` (comma-separated CORS allowlist)

## Available endpoints (scaffold)
- `GET /health` — basic liveness check.
- `GET /api/players?query=<term>` — database-backed player search (PostgreSQL) with optional `position`, `conference`, and `limit` filters. Requires `DB_URL` and assumes the `players`/`teams` tables are populated from an external feed (e.g., ESPN/NCAA) per `db/schema.sql`.

## Next steps
1. Add package/dependency setup for Drogon (FetchContent or system packages).
2. Flesh out route handlers for auth, leagues, players, drafts, and lineups.
3. Add integration tests for API endpoints and scoring logic.
4. Wire ingestion jobs to the Postgres schema in `db/schema.sql` and cache hot lookups in Redis.
