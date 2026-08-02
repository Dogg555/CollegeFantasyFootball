# One-time ESPN roster bootstrap

Use `scripts/espn_roster_ingest.py` to seed current 2026 Power Four rosters while CFBD's 2026 bulk roster response is empty.

The importer is intentionally separate from the normal CFBD pipeline because ESPN's site API is public but undocumented. It fetches only the ACC, Big 12, Big Ten, and SEC, writes a JSON audit file first, and then upserts the existing `players` table by ESPN athlete ID. It does not retire missing rows.

## Automatic Render run

After this change is merged, the next API deployment runs this sequence during Render's pre-deploy phase:

```sh
sh /srv/db/migrate.sh && python3 /srv/scripts/run_espn_roster_once.py
```

The one-time wrapper:

1. acquires a PostgreSQL advisory lock so overlapping deploys cannot start duplicate imports;
2. checks `ingestion_runs` for an existing successful `players_espn` row;
3. exits without making any ESPN requests when a successful import already exists;
4. otherwise runs the 2026 Power Four importer;
5. relies on the importer's successful ledger row to permanently disable future automatic runs.

A failed import causes the deployment to fail before the new API build goes live. Because no success marker is written, the next deployment can retry. After one successful import, later deployments execute only the inexpensive database check and never scrape ESPN again.

Render variables:

- `ESPN_ROSTER_AUTO_ONCE=true` enables the guarded automatic bootstrap.
- `ESPN_ROSTER_SEASON=2026` controls the season stored on imported players.
- `ESPN_ROSTER_ALLOW_PARTIAL=false` prevents database writes when any team roster fails.

Set `ESPN_ROSTER_AUTO_ONCE=false` to disable the hook manually. Deleting the successful `players_espn` ledger row would allow it to run again, so do that only intentionally.

## Install for a manual local run

```powershell
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r scripts/requirements-espn-ingest.txt
```

## Inspect before writing Postgres

```powershell
python scripts/espn_roster_ingest.py `
  --season 2026 `
  --dry-run `
  --output espn-rosters-2026.json
```

The command fails closed when a conference unexpectedly contains fewer than 10 or more than 25 teams, or when any team returns an empty/failed roster. The JSON export remains available for inspection.

## Import into Postgres manually

Set `DB_URL` to the database connection string. When running outside Render, use the database's external connection URL.

```powershell
$env:DB_URL = "postgresql://USER:PASSWORD@HOST/DATABASE"

python scripts/espn_roster_ingest.py `
  --season 2026 `
  --output espn-rosters-2026.json
```

A successful run prints inserted/updated counts and records an `ingestion_runs` row with resource `players_espn`. The automatic Render wrapper also recognizes a successful manual run and skips itself.

## Target one conference

```powershell
python scripts/espn_roster_ingest.py --season 2026 --conference sec --dry-run
```

Repeat `--conference` to select several conferences. Accepted values are `acc`, `big12`, `bigten`, and `sec`.

## Partial imports

By default, any team failure prevents all database writes. `--allow-partial` is available for an intentional partial import, but the default full-run behavior is safer.

## Returning to CFBD

Keep CFBD as the normal schedule, stats, and future roster provider. ESPN athlete IDs are stored directly in `players.id`; when CFBD later exposes the same upstream athlete ID, its normal upsert updates the existing row. Review the JSON audit export before reconciling any IDs that differ between providers.
