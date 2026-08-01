# Staging release checklist

## Render API environment

Set these values on `college-ff-api` before deploying:

- `ALLOWED_ORIGINS`: the exact staging frontend origin, including `https://` and no trailing slash.
- `JWT_SECRET`: a unique random value of at least 32 characters. The Blueprint uses `generateValue: true` for new services.
- `CFF_ADMIN_API_TOKEN`: a different unique random value of at least 32 characters. Copy the same value into GitHub Actions secret `CFF_STAGING_ADMIN_API_TOKEN`.
- `CFF_REQUIRE_EMAIL_VERIFICATION=false` until the broader beta email service is ready.

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
