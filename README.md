# College Fantasy Football (overview)

This repository contains a lightweight concept for a college fantasy football experience. It is split into a C++ backend scaffold and a static prototype frontend.

## Directory layout
- `backend/` — server scaffold and sample database schema.
- `frontend/` — static HTML/JS mock UI for sign-in, leagues, and player search.

## What to keep out of git
- Environment files: `.env`, `.env.local`, and any machine-specific variants.
- Secrets and certificates: TLS keys, API keys, and anything under `certs/`.
- Local databases and cache artifacts: `db/`, Postgres dumps, Redis snapshots.
- Generated assets and OS cruft: build outputs, IDE settings, and temp files.

If sensitive material accidentally lands in the repo, remove it and rotate any exposed keys immediately.
