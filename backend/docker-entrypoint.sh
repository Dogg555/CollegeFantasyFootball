#!/usr/bin/env sh
set -eu

sh /srv/db/migrate.sh

# Render Blueprint settings are not always synchronized to an existing service.
# Default the startup fallback to Render only when no explicit override exists.
if [ -z "${ESPN_ROSTER_AUTO_ONCE+x}" ]; then
  ESPN_ROSTER_AUTO_ONCE="${RENDER:-false}"
  export ESPN_ROSTER_AUTO_ONCE
fi

case "$(printf '%s' "${ESPN_ROSTER_AUTO_ONCE}" | tr '[:upper:]' '[:lower:]')" in
  1|true|yes|on)
    (
      echo "[espn-bootstrap] starting guarded background startup check"
      if ! python3 /srv/scripts/run_espn_roster_once.py; then
        echo "[espn-bootstrap] background import failed; API remains available and the next restart will resume checkpoints" >&2
      fi
    ) &
    ;;
  *)
    echo "[espn-bootstrap] startup fallback disabled"
    ;;
esac

exec /srv/college_ff_server
