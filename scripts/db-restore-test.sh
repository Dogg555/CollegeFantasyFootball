#!/usr/bin/env sh
set -eu

if [ -z "${BACKUP_FILE:-}" ] || [ ! -f "$BACKUP_FILE" ]; then
  echo "BACKUP_FILE must reference an existing custom-format dump" >&2
  exit 1
fi
if [ -z "${RESTORE_DB_URL:-}" ]; then
  echo "RESTORE_DB_URL is required" >&2
  exit 1
fi
if [ "${CFF_ALLOW_DESTRUCTIVE_RESTORE_TEST:-false}" != "true" ]; then
  echo "Set CFF_ALLOW_DESTRUCTIVE_RESTORE_TEST=true to confirm the target may be replaced" >&2
  exit 1
fi

TARGET_DB="$(psql "$RESTORE_DB_URL" -v ON_ERROR_STOP=1 -tAc 'SELECT current_database()')"
case "$TARGET_DB" in
  *_restore_test|*_test_restore|cff_restore_test) ;;
  *)
    echo "Refusing to restore into database '$TARGET_DB'; use a name ending in _restore_test or _test_restore" >&2
    exit 1
    ;;
esac

if [ -f "$BACKUP_FILE.sha256" ]; then
  (cd "$(dirname -- "$BACKUP_FILE")" && sha256sum -c "$(basename -- "$BACKUP_FILE.sha256")")
fi

pg_restore --list "$BACKUP_FILE" >/dev/null
pg_restore \
  --dbname="$RESTORE_DB_URL" \
  --clean \
  --if-exists \
  --no-owner \
  --no-privileges \
  --single-transaction \
  --exit-on-error \
  "$BACKUP_FILE"

psql "$RESTORE_DB_URL" -v ON_ERROR_STOP=1 <<'SQL'
DO $$
DECLARE
  required_table TEXT;
BEGIN
  FOREACH required_table IN ARRAY ARRAY[
    'schema_migrations', 'users', 'auth_tokens', 'leagues',
    'league_members', 'rosters', 'draft_states', 'transactions'
  ]
  LOOP
    IF to_regclass('public.' || required_table) IS NULL THEN
      RAISE EXCEPTION 'Restore is missing required table: %', required_table;
    END IF;
  END LOOP;
END
$$;
SQL

printf 'Restore validation succeeded for %s\n' "$TARGET_DB"
