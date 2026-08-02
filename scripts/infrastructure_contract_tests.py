#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[1]


def env_map(service: dict) -> dict[str, dict]:
    return {item["key"]: item for item in service.get("envVars", []) if item.get("key")}


def header_values(service: dict, path: str, name: str) -> list[str]:
    return [
        item.get("value", "")
        for item in service.get("headers", [])
        if item.get("path") == path and item.get("name") == name
    ]


def assert_digest_pinned(path: Path) -> None:
    from_lines = [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip().upper().startswith("FROM ")
    ]
    assert from_lines, f"{path} does not contain a FROM instruction"
    for line in from_lines:
        image = line.split()[1]
        assert "@sha256:" in image, f"{path} contains a mutable image reference: {line}"


def main() -> None:
    render = yaml.safe_load((ROOT / "render.yaml").read_text(encoding="utf-8"))
    services = {service["name"]: service for service in render["services"]}
    api = services["college-ff-api"]
    frontend = services["college-ff-frontend"]
    api_env = env_map(api)

    assert api_env["CFF_RUN_MIGRATIONS_ON_STARTUP"]["value"] == "false"
    assert api_env["ESPN_ROSTER_AUTO_ONCE"]["value"] == "false"
    assert api["preDeployCommand"].count("migrate.sh") == 1
    assert api["preDeployCommand"].count("run_espn_roster_once.py") == 1

    for path in ("/config.js", "/*.html", "/*.js", "/*.css"):
        values = header_values(frontend, path, "Cache-Control")
        assert values, f"missing Cache-Control for {path}"
        assert any("no-store" in value for value in values), (
            f"{path} must not serve stale application code"
        )
    asset_values = header_values(frontend, "/assets/*", "Cache-Control")
    assert asset_values and all("immutable" not in value for value in asset_values)

    compose = yaml.safe_load((ROOT / "docker-compose.yml").read_text(encoding="utf-8"))
    postgres = compose["services"]["postgres"]
    backend = compose["services"]["backend"]
    frontend_container = compose["services"]["frontend"]

    assert "@sha256:" in postgres["image"]
    assert postgres["ports"] == ["127.0.0.1:5432:5432"]
    assert postgres["healthcheck"]["test"][0] == "CMD-SHELL"
    assert backend["depends_on"]["postgres"]["condition"] == "service_healthy"
    assert frontend_container["depends_on"]["backend"]["condition"] == "service_healthy"
    assert "127.0.0.1:8080:8080" in backend["ports"]
    assert "127.0.0.1:3000:8080" in frontend_container["ports"]

    backend_env = set(backend["environment"])
    assert "CFF_RUN_MIGRATIONS_ON_STARTUP=true" in backend_env
    assert "ESPN_ROSTER_AUTO_ONCE=false" in backend_env
    assert not any(item.startswith("SSL_CERT_FILE=") for item in backend_env)
    assert not any(item.startswith("SSL_KEY_FILE=") for item in backend_env)

    nginx = (ROOT / "frontend" / "nginx.conf").read_text(encoding="utf-8")
    assert "proxy_pass http://backend:8080;" in nginx
    assert "proxy_ssl_verify off" not in nginx
    assert "proxy_connect_timeout 5s;" in nginx
    assert "proxy_send_timeout 30s;" in nginx
    assert "proxy_read_timeout 30s;" in nginx
    assert "proxy_next_upstream off;" in nginx
    assert 'Cache-Control "no-cache, no-store, must-revalidate"' in nginx

    entrypoint = (ROOT / "backend" / "docker-entrypoint.sh").read_text(encoding="utf-8")
    assert "CFF_RUN_MIGRATIONS_ON_STARTUP" in entrypoint
    assert "ESPN_ROSTER_AUTO_ONCE:-false" in entrypoint
    assert 'ESPN_ROSTER_AUTO_ONCE="${RENDER:-false}"' not in entrypoint

    migrations = (ROOT / "backend" / "db" / "migrate.sh").read_text(encoding="utf-8")
    assert "pg_isready --dbname=" in migrations
    assert "CFF_DB_WAIT_RETRIES" in migrations
    assert "CFF_DB_WAIT_SECONDS" in migrations

    health_source = ROOT / "backend" / "src" / "health_status.cpp"
    assert health_source.is_file()
    health_text = health_source.read_text(encoding="utf-8")
    assert "k503ServiceUnavailable" in health_text
    assert 'payload["status"].asString() == "ok"' in health_text
    cmake = (ROOT / "backend" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "src/health_status.cpp" in cmake

    for dockerfile in (
        ROOT / "frontend" / "Dockerfile",
        ROOT / "backend" / "Dockerfile",
        ROOT / "ops" / "backup" / "Dockerfile",
    ):
        assert_digest_pinned(dockerfile)

    print("Infrastructure hardening contracts passed.")


if __name__ == "__main__":
    main()
