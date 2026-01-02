# Backend snapshot

This folder holds the server scaffold for the project. It is intentionally minimal and meant for local exploration.

## Contents
- `src/` — HTTP entry point, in-memory auth mock, and placeholder domain handlers.
- `Dockerfile` and `CMakeLists.txt` — build and container hints for local runs.

## Safety reminders
- Keep `.env`, `.env.local`, TLS keys, and database credentials out of version control.
- Avoid committing database dumps or generated binaries; prefer a clean build each time.
- Remove any stray certificates or secret keys from the repo history if they appear.

## Quick build hint
With Drogon and dependencies available, configure and build using CMake into `build/`, or run through the provided Dockerfile for an isolated test run. Configure environment variables like `JWT_SECRET`, `DB_URL`, and `ALLOWED_ORIGINS` at runtime rather than hardcoding them.

## CFBD ingestion (players)
The backend can fetch player data directly from the CollegeFootballData API and persist it into Postgres.

Triggers:
- Admin HTTP: `POST /api/admin/ingest/cfbd` (requires the same bearer token used for other secure endpoints).

