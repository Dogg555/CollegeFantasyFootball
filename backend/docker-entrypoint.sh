#!/usr/bin/env sh
set -eu

is_enabled() {
  case "$(printf '%s' "${1:-false}" | tr '[:upper:]' '[:lower:]')" in
    1|true|yes|on) return 0 ;;
    *) return 1 ;;
  esac
}

if is_enabled "${CFF_RUN_MIGRATIONS_ON_STARTUP:-true}"; then
  echo "[startup] applying database migrations"
  sh /srv/db/migrate.sh
else
  echo "[startup] migrations skipped; deployment lifecycle owns migrations"
fi

if is_enabled "${ESPN_ROSTER_AUTO_ONCE:-false}"; then
  (
    echo "[espn-bootstrap] starting explicit guarded startup check"
    if ! python3 /srv/scripts/run_espn_roster_once.py; then
      echo "[espn-bootstrap] startup import failed; API remains available and checkpoints are preserved" >&2
    fi
  ) &
else
  echo "[espn-bootstrap] startup fallback disabled"
fi

exec /srv/college_ff_server
