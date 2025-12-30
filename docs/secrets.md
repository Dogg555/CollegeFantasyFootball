# Secrets handling (local, CI, containers)

Keep all secrets out of the repo. Use environment variables and secret stores instead of hardcoded values.

## Local development
- Copy `.env.example` to `.env.local` (gitignored) and fill in values for `POSTGRES_USER`, `POSTGRES_PASSWORD`, `DB_URL`, `JWT_SECRET`, `CFBD_API_KEY`, etc.
- `docker compose` will read `.env.local` automatically. Do **not** commit `.env.local`.
- For certificates, set `CERTS_DIR` to a host path **outside** the repo and mount it (default is `./certs` for convenience, but move certs elsewhere in real use).

## Containers (docker compose)
- `docker-compose.yml` now references env vars only; there are no embedded credentials.
- Use a gitignored override (e.g., `docker-compose.override.yml`) or `.env.local` to provide secrets at runtime.
- Mount certs via `CERTS_DIR` and point `SSL_CERT_FILE` / `SSL_KEY_FILE` to the mounted paths inside the container.

## GitHub Actions / CI
- Store secrets in GitHub: **Settings → Secrets and variables → Actions → New repository secret**.
- Recommended secrets:
  - `CFBD_API_KEY`
  - `DB_URL` (includes Postgres user/password/host/db)
  - `JWT_SECRET`
  - `POSTGRES_USER` / `POSTGRES_PASSWORD` / `POSTGRES_DB` if your workflow provisions Postgres
  - `SSL_CERT_FILE` / `SSL_KEY_FILE` contents as base64 if you need to provide certs in CI; decode them in the workflow before starting services.
- Example GitHub Actions snippet to expose secrets as env vars:
  ```yaml
  env:
    CFBD_API_KEY: ${{ secrets.CFBD_API_KEY }}
    DB_URL: ${{ secrets.DB_URL }}
    JWT_SECRET: ${{ secrets.JWT_SECRET }}
  ```
- For certs in CI:
  ```yaml
  - name: Write TLS certs
    run: |
      echo "${{ secrets.SSL_CERT_PEM_B64 }}" | base64 -d > /tmp/cert.crt
      echo "${{ secrets.SSL_KEY_PEM_B64 }}" | base64 -d > /tmp/cert.key
    env:
      SSL_CERT_FILE: /tmp/cert.crt
      SSL_KEY_FILE: /tmp/cert.key
  ```

## Operational notes
- Never check secrets, certs, or override files into git.
- Avoid logging secret values; the ingestor already avoids echoing keys/URLs.
- Rotate keys periodically and immediately if exposure is suspected.
