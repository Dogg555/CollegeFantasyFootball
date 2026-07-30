# Contributing

Thanks for helping improve College Fantasy Football.

## Local Setup
1. Copy `.env.example` to `.env.local` and fill in local values.
2. Start Postgres with Docker Compose or provide your own `DB_URL`.
3. Run migrations:
   ```sh
   sh backend/db/migrate.sh
   ```
4. Build the backend:
   ```sh
   cmake --build backend/build --config Debug
   ```
5. Run smoke tests against a running backend:
   ```sh
   python scripts/api_smoke_tests.py
   ```

## Development Rules
- Keep secrets out of git. Use `.env.local`, Render env vars, or GitHub Actions secrets.
- Add DB changes as versioned files under `backend/db/migrations/` and update `backend/db/schema.sql`.
- Keep production defaults safe:
  - `CFF_REQUIRE_DB=true`
  - `CFF_ALLOW_SHARED_SECRET_AUTH=false`
  - `CFF_EXPOSE_AUTH_TOKENS=false`
  - `CFF_LOG_AUTH_TOKENS=false`
- Do not commit generated build outputs, local database files, certs, or API keys.

## Checks Before a PR
Run the checks available on your machine:

```sh
node --check frontend/state.js
node --check frontend/auth.js
node --check frontend/league.js
node --check frontend/draft.js
cmake --build backend/build --config Debug
```

CI also builds the backend Docker image, starts Postgres, runs migrations, and executes `scripts/api_smoke_tests.py`.

## Security
Do not open public issues for vulnerabilities. Follow `SECURITY.md`.
