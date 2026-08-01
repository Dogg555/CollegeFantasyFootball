#!/usr/bin/env sh
set -eu

if [ -z "${DB_URL:-}" ]; then
  echo "DB_URL is required" >&2
  exit 1
fi

umask 077
BACKUP_DIR="${BACKUP_DIR:-./backups}"
BACKUP_PREFIX="${BACKUP_PREFIX:-college-ff}"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
BACKUP_FILE="${BACKUP_FILE:-$BACKUP_DIR/$BACKUP_PREFIX-$TIMESTAMP.dump}"

mkdir -p "$(dirname -- "$BACKUP_FILE")"

cleanup() {
  if [ -f "$BACKUP_FILE.tmp" ]; then
    rm -f "$BACKUP_FILE.tmp"
  fi
}
trap cleanup EXIT HUP INT TERM

pg_dump \
  --dbname="$DB_URL" \
  --format=custom \
  --compress=9 \
  --no-owner \
  --no-privileges \
  --file="$BACKUP_FILE.tmp"

pg_restore --list "$BACKUP_FILE.tmp" >/dev/null
mv "$BACKUP_FILE.tmp" "$BACKUP_FILE"
backup_dir="$(dirname -- "$BACKUP_FILE")"
backup_name="$(basename -- "$BACKUP_FILE")"
(cd "$backup_dir" && sha256sum "$backup_name" > "$backup_name.sha256")
chmod 600 "$BACKUP_FILE" "$BACKUP_FILE.sha256"

printf '%s\n' "$BACKUP_FILE"
