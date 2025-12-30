# CFBD Ingestion (CollegeFootballData API)

This repo includes a helper script to pull teams and rosters from [collegefootballdata.com](https://collegefootballdata.com) and upsert them into Postgres.

## Prerequisites
- Python 3.9+
- `requests` and `psycopg2-binary` (install via `pip install -r scripts/requirements-ingest.txt`)
- A running Postgres instance with the schema from `backend/db/schema.sql` applied.
- Environment variables:
  - `CFBD_API_KEY` — API key from [CollegeFootballData](https://collegefootballdata.com).
  - `DB_URL` — Postgres connection string (e.g., `postgres://cff:cffpass@localhost:5432/cff`).
  - Optional: `CFBD_SEASON` (defaults to current year), `CFBD_DELAY_SEC` (default `0.2` seconds between HTTP calls).

## Running locally
```bash
pip install -r scripts/requirements-ingest.txt
export CFBD_API_KEY=your_cfbd_key
export DB_URL=postgres://cff:cffpass@localhost:5432/cff
export CFBD_SEASON=2024   # optional
python scripts/ingest_cfbd.py
```

## Notes
- Teams are upserted before players; provider IDs are stored under `provider_id=1` to identify CFBD as the data source.
- Player rows include `full_name`, `position`, `class`, `conference`, and the full CFBD payload in `raw_payload`.
- Reruns are idempotent thanks to `ON CONFLICT` upserts keyed on `provider_team_id` and `provider_player_id`.
