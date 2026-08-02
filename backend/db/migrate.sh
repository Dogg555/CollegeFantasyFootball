#!/usr/bin/env sh
set -eu

if [ -z "${DB_URL:-}" ]; then
  echo "DB_URL is required" >&2
  exit 1
fi

DB_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
LOCK_NAME="college-fantasy-football-schema-migrations"
DB_WAIT_RETRIES="${CFF_DB_WAIT_RETRIES:-30}"
DB_WAIT_SECONDS="${CFF_DB_WAIT_SECONDS:-2}"

case "$DB_WAIT_RETRIES" in
  ''|*[!0-9]*) echo "CFF_DB_WAIT_RETRIES must be a positive integer" >&2; exit 1 ;;
esac
case "$DB_WAIT_SECONDS" in
  ''|*[!0-9]*) echo "CFF_DB_WAIT_SECONDS must be a positive integer" >&2; exit 1 ;;
esac
if [ "$DB_WAIT_RETRIES" -lt 1 ] || [ "$DB_WAIT_SECONDS" -lt 1 ]; then
  echo "Database wait settings must be greater than zero" >&2
  exit 1
fi

attempt=1
until pg_isready --dbname="$DB_URL" >/dev/null 2>&1; do
  if [ "$attempt" -ge "$DB_WAIT_RETRIES" ]; then
    echo "PostgreSQL was not ready after ${DB_WAIT_RETRIES} attempts" >&2
    exit 1
  fi
  echo "Waiting for PostgreSQL (${attempt}/${DB_WAIT_RETRIES})..."
  sleep "$DB_WAIT_SECONDS"
  attempt=$((attempt + 1))
done

psql "$DB_URL" -v ON_ERROR_STOP=1 -c "
CREATE TABLE IF NOT EXISTS schema_migrations (
  version TEXT PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
"

apply_migration() {
  version="$1"
  migration="$2"

  case "$version" in
    *[!A-Za-z0-9_.-]*|'')
      echo "Unsafe migration version: $version" >&2
      exit 1
      ;;
  esac

  echo "Checking $version"
  {
    printf "SELECT pg_advisory_xact_lock(hashtext('%s'));\n" "$LOCK_NAME"
    printf "%s\n" "SELECT NOT EXISTS (SELECT 1 FROM schema_migrations WHERE version = '$version') AS apply_migration \\gset"
    printf '%s\n' '\if :apply_migration'
    printf '%s\n' "\\echo Applying $version"
    cat "$migration"
    printf "\nINSERT INTO schema_migrations (version) VALUES ('%s');\n" "$version"
    printf '%s\n' '\else'
    printf '%s\n' "\\echo Skipping $version"
    printf '%s\n' '\endif'
  } | psql "$DB_URL" -v ON_ERROR_STOP=1 --single-transaction
}

apply_migration "001_schema_snapshot" "$DB_DIR/schema.sql"

for migration in "$DB_DIR"/migrations/*.sql; do
  [ -e "$migration" ] || continue
  version="$(basename "$migration" .sql)"
  apply_migration "$version" "$migration"
done
