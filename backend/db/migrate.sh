#!/usr/bin/env sh
set -eu

if [ -z "${DB_URL:-}" ]; then
  echo "DB_URL is required" >&2
  exit 1
fi

DB_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

psql "$DB_URL" -v ON_ERROR_STOP=1 -c "
CREATE TABLE IF NOT EXISTS schema_migrations (
  version TEXT PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
"

if ! psql "$DB_URL" -v ON_ERROR_STOP=1 -tAc "SELECT 1 FROM schema_migrations WHERE version = '001_schema_snapshot'" | grep -q 1; then
  echo "Applying 001_schema_snapshot"
  psql "$DB_URL" -v ON_ERROR_STOP=1 -f "$DB_DIR/schema.sql"
  psql "$DB_URL" -v ON_ERROR_STOP=1 -c "INSERT INTO schema_migrations (version) VALUES ('001_schema_snapshot') ON CONFLICT DO NOTHING;"
fi

for migration in "$DB_DIR"/migrations/*.sql; do
  [ -e "$migration" ] || continue
  version="$(basename "$migration" .sql)"
  if psql "$DB_URL" -v ON_ERROR_STOP=1 -tAc "SELECT 1 FROM schema_migrations WHERE version = '$version'" | grep -q 1; then
    echo "Skipping $version"
    continue
  fi
  echo "Applying $version"
  psql "$DB_URL" -v ON_ERROR_STOP=1 -f "$migration"
  psql "$DB_URL" -v ON_ERROR_STOP=1 -c "INSERT INTO schema_migrations (version) VALUES ('$version');"
done
