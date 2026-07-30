# Render Smoke Checklist

Use this after every Render deploy.

## Environment
- `DB_URL` is attached from Render Postgres.
- `JWT_SECRET` is set.
- `ALLOWED_ORIGINS` includes the deployed frontend origin.
- `CFBD_API_KEY` is set before running ingestion.
- `CFF_REQUIRE_DB` is `true`.
- `CFF_ALLOW_SHARED_SECRET_AUTH` is `false`.
- `CFF_EXPOSE_AUTH_TOKENS` is `false`.
- `CFF_LOG_AUTH_TOKENS` is `false`.

## Backend Checks
```sh
curl https://YOUR-RENDER-SERVICE.onrender.com/health
curl https://YOUR-RENDER-SERVICE.onrender.com/api/health
```

Expected: JSON with `status: "ok"` and `database: "ok"`.

Automated run:
```sh
CFF_API_BASE_URL=https://YOUR-RENDER-SERVICE.onrender.com python scripts/api_smoke_tests.py
```

Create or sign in:
```sh
curl -X POST https://YOUR-RENDER-SERVICE.onrender.com/api/auth/signup \
  -H "Content-Type: application/json" \
  -d '{"email":"admin@example.com","password":"change-me-now"}'
```

Save the returned token, then check auth-backed routes:
```sh
curl https://YOUR-RENDER-SERVICE.onrender.com/api/leagues \
  -H "Authorization: Bearer YOUR_TOKEN"

curl https://YOUR-RENDER-SERVICE.onrender.com/api/admin/ingest/cfbd/status \
  -H "Authorization: Bearer YOUR_TOKEN"
```

## Data Ingestion
Run this only after `CFBD_API_KEY` is configured:
```sh
curl -X POST https://YOUR-RENDER-SERVICE.onrender.com/api/admin/ingest/cfbd \
  -H "Authorization: Bearer YOUR_TOKEN"
```

Then refresh:
```sh
curl https://YOUR-RENDER-SERVICE.onrender.com/api/admin/ingest/cfbd/status \
  -H "Authorization: Bearer YOUR_TOKEN"
```

Expected:
- `status` is `ok` or `partial`.
- `counts.players` increases after a successful player import.
- Recent runs are visible in the Players page Data Ingestion panel.

## Frontend Flow
- Sign up or sign in.
- Create a league.
- Invite a manager.
- Open league settings as commissioner.
- Open draft lobby.
- Queue a player from Players.
- Enter draft room and make a pick.
- Submit a waiver claim.
- Send and cancel a trade offer.
