#!/usr/bin/env bash
set -euo pipefail

# Installs PostgreSQL and pgAdmin on Debian/Ubuntu hosts.
# - PostgreSQL: official Debian package
# - pgAdmin: official repo (desktop/web packages available)
#
# Usage:
#   ./scripts/install_postgres_pgadmin.sh
#   sudo ./scripts/install_postgres_pgadmin.sh   # if your user lacks package privileges
#
# After installation:
# - PostgreSQL runs as a service: sudo systemctl status postgresql
# - pgAdmin (web) requires initial setup: sudo /usr/pgadmin4/bin/setup-web.sh

if [[ "${EUID}" -ne 0 ]]; then
  SUDO="sudo"
else
  SUDO=""
fi

echo "[install] Updating apt cache and installing prerequisites..."
$SUDO apt-get update -y
$SUDO apt-get install -y --no-install-recommends curl gnupg ca-certificates lsb-release

echo "[install] Installing PostgreSQL..."
$SUDO apt-get install -y postgresql postgresql-contrib

echo "[install] Adding pgAdmin APT repo..."
DISTRO_CODENAME="$(lsb_release -cs)"
curl -fsS https://www.pgadmin.org/static/packages_pgadmin_org.pub | $SUDO gpg --dearmor -o /usr/share/keyrings/pgadmin.gpg
echo "deb [signed-by=/usr/share/keyrings/pgadmin.gpg] https://ftp.postgresql.org/pub/pgadmin/pgadmin4/apt/${DISTRO_CODENAME} pgadmin4 main" | \
  $SUDO tee /etc/apt/sources.list.d/pgadmin4.list > /dev/null

echo "[install] Installing pgAdmin4 (web + desktop)..."
$SUDO apt-get update -y
$SUDO apt-get install -y pgadmin4 pgadmin4-web pgadmin4-desktop

echo "[install] Enabling PostgreSQL service..."
$SUDO systemctl enable postgresql
$SUDO systemctl start postgresql

cat <<'EOF'
[install] Done.
- PostgreSQL service: sudo systemctl status postgresql
- Initialize pgAdmin (web): sudo /usr/pgadmin4/bin/setup-web.sh
- pgAdmin desktop: launch "pgadmin4" from your application menu.

Default Postgres peer auth may require `sudo -u postgres psql` for first-time setup.
EOF
