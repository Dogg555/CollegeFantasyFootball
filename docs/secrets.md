# Secrets handling (local, CI, containers)

Keep all secrets out of the repo. Use environment variables and secret stores instead of hardcoded values. If anything sensitive is ever committed, rotate it immediately and scrub it from history.

## Immediate incident response
1. **Rotate credentials** that may have been exposed (DB user/password, JWT signing key, CFBD API key, TLS private keys).
2. **Scan history** to identify leaks:
   - `gitleaks detect --source . --report-path gitleaks-report.json --redact`
   - `trufflehog git file://$(pwd) --json | tee trufflehog-report.json`
3. **Rewrite history** if a secret was committed:
   - Install git-filter-repo (`pip install git-filter-repo`) and remove the offending files or replace text:
     ```
     git filter-repo --path secrets.txt --invert-paths
     # or to overwrite a leaked value everywhere
     git filter-repo --replace-text replacements.txt
     ```
   - Force-push (`git push --force-with-lease`) and notify collaborators to reclone.
4. **Rebuild images** and restart services with rotated values so stale images no longer contain secrets.

## Local development
- Copy `.env.example` to `.env.local` (gitignored) and fill in values for `POSTGRES_USER`, `POSTGRES_PASSWORD`, `DB_URL`, `JWT_SECRET`, `CFBD_API_KEY`, etc.
- `docker compose` will read `.env.local` automatically. Do **not** commit `.env.local` or any certs.
- For certificates, set `CERTS_DIR` to a host path **outside** the repo and mount it (default is `./certs` for convenience, but move certs elsewhere in real use).

## Containers (docker compose)
- `docker-compose.yml` references env vars only; there are no embedded credentials.
- Use a gitignored override (e.g., `docker-compose.override.yml`) or `.env.local` to provide secrets at runtime.
- Mount certs via `CERTS_DIR` and point `SSL_CERT_FILE` / `SSL_KEY_FILE` to the mounted paths inside the container.
- Avoid copying `.env*` or `certs/` into images; mount them instead.

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

## Preventing future leaks
- Use the provided `.pre-commit-config.yaml` to enable `detect-secrets` and `gitleaks` locally.
- Keep `.env`, `.env.local`, `.env.*`, `certs/`, and `.secrets/` out of git; they are gitignored.
- Run `gitleaks detect --source .` locally before opening a PR.
- Never paste JWT signing keys or database URLs into client-side code, logs, or markdown.

## Operational notes
- Avoid logging secret values; the ingestor already avoids echoing keys/URLs.
- Rotate keys periodically and immediately if exposure is suspected.
