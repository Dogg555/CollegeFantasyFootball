# Staging release checklist

## Render API environment

Set these values on `college-ff-api` before deploying:

- `ALLOWED_ORIGINS`: the exact staging frontend origin, including `https://` and no trailing slash.
- `JWT_SECRET`: a unique random value of at least 32 characters. The Blueprint uses `generateValue: true` for new services.
- `CFF_ADMIN_API_TOKEN`: a different unique random value of at least 32 characters. Copy the same value into GitHub Actions secret `CFF_STAGING_ADMIN_API_TOKEN`.
- `CFF_REQUIRE_EMAIL_VERIFICATION=false` until the broader beta email service is ready.
- `CFBD_API_KEY`: the CollegeFootballData API key used by the daily player ingestion job.

The staging workflow sends one request with the configured frontend origin and one with an untrusted origin. It fails unless the exact origin is allowed and the untrusted origin is rejected.

## GitHub Actions secrets

Required for the email-deferred staging run:

- `RENDER_STAGING_API_KEY`
- `RENDER_STAGING_SERVICE_ID`
- `CFF_STAGING_API_BASE_URL`
- `CFF_STAGING_FRONTEND_BASE_URL`
- `CFF_STAGING_ADMIN_API_TOKEN`

Only required when the workflow input **Include email-verification acceptance checks** is enabled:

- `CFF_STAGING_SMOKE_EMAIL`
- `CFF_STAGING_SMOKE_PASSWORD`
- `CFF_STAGING_VERIFICATION_EMAIL_BASE`

## Desktop and mobile review

The workflow tests Chromium at 1440×900 and 390×844, checks for load failures, browser errors, horizontal overflow, mobile navigation, and password-policy drift, then uploads full-page screenshots for 14 days. Review the screenshot artifact for visual issues that automated checks cannot judge.

## Daily CFBD data ingestion

The Blueprint defines `college-ff-cfbd-ingest`, a Render cron job that runs every day at `10:00 UTC`.

The job:

1. Connects to `college-ff-api` over Render's private network.
2. Reuses the API service's generated `CFF_ADMIN_API_TOKEN` through a Blueprint service reference.
3. Checks `/health` and `/api/health` before ingestion.
4. Calls `POST /api/admin/ingest/cfbd` once with a 15-minute timeout.
5. Reads `GET /api/admin/ingest/cfbd/status` after completion.
6. Exits nonzero when the endpoint reports a partial or failed ingest, so Render records a failed run.

The API's startup and in-process interval ingestion are disabled in the Blueprint to prevent duplicate schedules. Keep `CFBD_INGEST_ON_STARTUP=false` and do not set `CFBD_INGEST_INTERVAL_HOURS` on the API service.

After syncing the Blueprint, open the cron job's **Runs** page and trigger one manual run. Confirm:

- both health checks report `status=ok` and `database=ok`;
- the ingest response reports `status=ok`;
- the status response contains the latest completed run;
- the next scheduled run is shown for `10:00 UTC`;
- the Players page returns current CFBD data.

Render schedules cron expressions in UTC. A dedicated Render cron service has a minimum monthly charge and should exit after the ingestion finishes.

## Off-platform database backups

The Blueprint defines a daily Render cron job at `07:00 UTC`. Configure these cron-job environment variables:

- `CFF_BACKUP_S3_BUCKET`
- `AWS_ACCESS_KEY_ID`
- `AWS_SECRET_ACCESS_KEY`
- `AWS_REGION` (`auto` for Cloudflare R2)
- `AWS_ENDPOINT_URL` for an S3-compatible provider; omit for AWS S3
- `CFF_BACKUP_S3_PREFIX` (default `college-ff/postgres`)
- `CFF_BACKUP_RETENTION_DAYS` (default `30`, minimum `7`)
- `CFF_BACKUP_SSE` (`AES256`, or `aws:kms` for AWS KMS)
- `CFF_BACKUP_KMS_KEY_ID` only when using `aws:kms`

Each run creates a compressed PostgreSQL custom-format dump, calculates SHA-256, uploads the dump and checksum over TLS with server-side encryption enabled, and deletes expired backup objects under the configured prefix.

Trigger the cron job once manually after adding credentials. Confirm both `.dump` and `.dump.sha256` objects exist, then perform a test restore using `scripts/db-restore-test.sh` before treating backups as operational.
