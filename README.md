# College Fantasy Football

A full-stack concept for a college fantasy football platform covering Division I FBS and FCS players. The backend is written in C++ and the frontend uses lightweight HTML/CSS/JavaScript.

## Structure
- `docs/architecture.md` — architecture overview and roadmap.
- `backend/` — C++ backend scaffold (Drogon-ready) with CMake config.
- `frontend/` — static UI prototype for live scores, player search, and league summary.

## Quickstart
1. Review `docs/architecture.md` for the proposed system design.
2. Install dependencies:
   - Debian/Ubuntu: `./scripts/install_dependencies.sh`
   - Windows 11: run `./scripts/install_windows.ps1` (PowerShell) or see `docs/windows-setup.md` (vcpkg/WSL options)
   ```bash
   # Debian/Ubuntu example
   ./scripts/install_dependencies.sh
   ```
   - Optional local database tooling:
     - Linux: `./scripts/install_postgres_pgadmin.sh` (installs PostgreSQL + pgAdmin)
     - Windows: `powershell -ExecutionPolicy Bypass -File .\scripts\install_postgres_pgadmin.ps1` (winget install)
3. Build the backend (requires Drogon and CMake):
   ```bash
   cmake -S backend -B backend/build -DDROGON_FOUND=ON
   cmake --build backend/build
   ./backend/build/college_ff_server
   ```
   - Environment variables: `PORT` (default `8080`), `JWT_SECRET` (required for secure endpoints), optional `SSL_CERT_FILE`/`SSL_KEY_FILE` for HTTPS, and `ALLOWED_ORIGINS` (comma-separated) for CORS.
4. Serve the frontend locally (any static server, e.g., `python -m http.server` from `frontend/`).
5. Optional: run everything via Docker for local testing:
   ```bash
   docker compose up --build
   ```
   - Backend available at `http://localhost:8080`.
   - Frontend available at `http://localhost:3000` (includes `test.html` to hit the secure ping endpoint).
6. Optional: ingest CFBD data into Postgres for player search. See `docs/ingestion_cfbd.md` and run `python scripts/ingest_cfbd.py` after applying the schema in `backend/db/schema.sql`.

## Roadmap
- Implement auth, league creation, player search, and draft APIs in C++.
- Wire the frontend to real backend endpoints for live scores and player search.
- Add data ingestion for schedules, rosters, and live stats.
