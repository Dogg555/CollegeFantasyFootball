# Durable Live-Stat Worker

The CFBD live-score adapter now has a persistent worker boundary instead of relying only on an in-process cache refresh.

## Operator endpoints

Both endpoints require the existing administrator authorization policy.

### Start a refresh

`POST /api/admin/live-stats/run`

Optional JSON body:

```json
{
  "season": 2026,
  "week": 1,
  "force": false,
  "runKey": "manual-week-1"
}
```

The same fields may be supplied as query parameters. The response reports whether the run was accepted, skipped as a recent duplicate, or rejected because a matching run is already active.

### Inspect worker state

`GET /api/admin/live-stats/status`

Optional `season` and `week` query parameters narrow the durable run and freshness history. The response includes:

- recent `stat_ingest_runs`
- per-run source results
- source freshness and consecutive failure state
- scoring refresh queue counts
- recent operator events
- existing live-score cache health
- capability flags showing which provider adapters and downstream workers are active

## Scheduling

The worker is disabled unless one of these settings is enabled:

- `CFF_LIVE_STAT_ON_STARTUP=true`
- `CFF_LIVE_STAT_INTERVAL_MINUTES=<positive integer>`

Scope defaults come from `CFBD_SEASON` and `CFF_CURRENT_WEEK`. When no season is configured, the worker derives the current college-football season. A missing week uses `0`, meaning the existing scoreboard/schedule cache scope.

## Reliability controls

- `CFF_LIVE_STAT_MAX_ATTEMPTS` defaults to `3` and is capped at `5`.
- `CFF_LIVE_STAT_RETRY_BASE_MS` defaults to `750` milliseconds and uses exponential backoff.
- `CFF_LIVE_STAT_DEDUPE_MINUTES` defaults to `2` minutes and suppresses accidental repeated successful or failed runs.
- The database unique active-scope constraint prevents two workers from processing the same provider, season, and week concurrently.
- Source results, freshness, retry events, and final status survive process restarts.

## Scoring safety gate

The current adapter refreshes CFBD scoreboard and schedule cache data only. It does not yet ingest authoritative per-player weekly statistics, calculate material stat changes, or process `scoring_refresh_queue` jobs.

For that reason, this worker intentionally reports:

- `playerStatsAdapter: false`
- `scoringRefreshWorker: false`
- `scoringRefreshReady: false`

Do not enable fantasy rescoring from scoreboard-only changes. The next implementation slice should add the player-stat provider adapter, persist normalized stat rows, detect changed scoring inputs, and then enqueue and execute idempotent scoring refresh jobs.
