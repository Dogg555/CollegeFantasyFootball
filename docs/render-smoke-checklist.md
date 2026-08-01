# Render Smoke Checklist

Use this after every Render deploy.

## Environment
Backend service:
- `DB_URL` is attached from Render Postgres.
- `JWT_SECRET` is set.
- `ALLOWED_ORIGINS` includes the deployed frontend origin.
- `CFBD_API_KEY` is set before running ingestion.
- `CFF_ADMIN_API_TOKEN` is set to a high-entropy private token.
- `CFF_ADMIN_EMAILS` lists any signed-in accounts allowed to use admin endpoints.
- `CFBD_INGEST_ON_STARTUP` is `true` if data should refresh on API startup.
- `CFBD_INGEST_INTERVAL_HOURS` is set to the desired background refresh interval, for example `24`.
- `CFF_REQUIRE_DB` is `true`.
- `RESEND_API_KEY` is set if password reset/email verification should send real email.
- `CFF_EMAIL_FROM` is a verified sender.
- `CFF_FRONTEND_BASE_URL` is the deployed frontend URL.
- `CFF_ALLOW_SHARED_SECRET_AUTH` is `false`.
- `CFF_EXPOSE_AUTH_TOKENS` is `false`.
- `CFF_LOG_AUTH_TOKENS` is `false`.

Frontend service:
- `CFF_API_BASE` is the deployed API URL plus `/api`.
- `CFF_ALLOW_LOCAL_DEMO` is `false`.
- Render build logs show `Frontend build ready in frontend-dist`.

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

Or run the same smoke coverage with Node:
```sh
CFF_API_BASE_URL=https://YOUR-RENDER-SERVICE.onrender.com node scripts/api_smoke_tests.mjs
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
Ingestion is not exposed in the public frontend. For normal hosted operation, let the API run it behind the scenes with `CFBD_INGEST_ON_STARTUP` and `CFBD_INGEST_INTERVAL_HOURS`.

Run this manually only from a trusted operational terminal after `CFBD_API_KEY` is configured:
```sh
CFF_API_BASE_URL=https://YOUR-RENDER-SERVICE.onrender.com \
CFF_ADMIN_API_TOKEN=YOUR_ADMIN_TOKEN \
python scripts/ops_ingest.py --run
```

Then refresh:
```sh
CFF_API_BASE_URL=https://YOUR-RENDER-SERVICE.onrender.com \
CFF_ADMIN_API_TOKEN=YOUR_ADMIN_TOKEN \
python scripts/ops_ingest.py
```

Expected:
- Public `/health` and `/api/health` checks succeed in the helper output.
- `status` is `ok` or `partial`.
- `counts.players` increases after a successful player import.
- Recent runs are visible through the admin status endpoint.
- A normal signed-in user not listed in `CFF_ADMIN_EMAILS` receives `403` from `/api/admin/ingest/cfbd/status`.

## Frontend Flow
- Open the deployed frontend static service URL.
- In browser dev tools, confirm `window.CFF_API_BASE` points to the deployed API service.
- Create an account on `signup.html`.
- Verify email on `verify-email.html` if required.
- Request a new verification email on `resend-verification.html`.
- Sign in on `signin.html`.
- Use `reset-request.html` and `reset-password.html` for password recovery.
- Create a league.
- Invite a manager.
- Open league settings as commissioner.
- Open draft lobby.
- In Draft Room, randomize or reset draft order before the first pick.
- Queue a player from Players.
- Enter draft room and make a pick.
- As commissioner, undo the last draft pick and confirm the player leaves the roster and the draft returns to that pick.
- Submit a waiver claim.
- Send and cancel a trade offer.
