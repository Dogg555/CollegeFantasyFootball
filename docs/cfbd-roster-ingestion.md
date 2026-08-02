# CFBD roster ingestion

The weekly player refresh uses three CFBD requests:

1. `GET /info` to verify that the authenticated key has enough remaining calls.
2. `GET /teams/fbs?year=<season>` to build the team and conference map.
3. `GET /roster?year=<season>&classification=fbs` to download all FBS players in one bulk response.

The previous implementation requested `/roster` once per team. A single refresh therefore used roughly 130–140 calls and could continue issuing requests after CFBD returned `429`. The bulk endpoint reduces the normal refresh to three calls.

## 429 behavior

A `429` stops the refresh immediately. The ingestion run is recorded as partial, no empty import is written, and the existing active player catalog is preserved. The error message includes the API response detail and `Retry-After` when present.

CFBD enforces monthly call limits. When `/info` reports fewer than three remaining calls, the job exits before requesting team or roster data. The error includes `resetAt` when CFBD provides it. Wait until that reset or increase the API tier before manually retrying.

## Verification

After a successful run, check:

- `GET /api/admin/ingest/cfbd/status` returns top-level `status: "ok"` and `configured: true`.
- In that response, the newest player entry in `runs` has `status: "success"`, `rowCount` greater than zero, and a small `apiCalls` value (normally three).
- `GET /api/players/meta` reports active players, the expected season, and FBS team coverage.
- If the newest player run is `partial`, inspect its `error` field for quota, authentication, network, or roster-coverage details before retrying.
