# Alpha release runbook

College Fantasy Football remains **pre-alpha** until the deployed Alpha workflow passes and the manual pilot evidence is recorded.

## Repository and Render configuration

The Render Blueprint defines:

- `college-ff-api`
- `college-ff-frontend`
- weekly `college-ff-cfbd-ingest`
- seasonal two-minute `college-ff-live-ingest`
- daily encrypted `college-ff-db-backup`
- PostgreSQL `college-ff-db`

The full roster sync runs weekly at `0 8 * * 1` UTC. Use Render **Trigger Run** for preseason, transfer, or emergency refreshes. Live score polling remains separate.

## Required GitHub Actions secrets

- `CFF_STAGING_API_BASE_URL`
- `CFF_STAGING_FRONTEND_BASE_URL`
- `CFF_STAGING_ADMIN_API_TOKEN`
- `CFF_STAGING_SMOKE_EMAIL`
- `CFF_STAGING_SMOKE_PASSWORD`
- `CFF_STAGING_VERIFICATION_EMAIL_BASE`

The existing Render deployment workflow additionally requires:

- `RENDER_STAGING_API_KEY`
- `RENDER_STAGING_SERVICE_ID`

## Required Render variables

API:

- `DB_URL`
- `JWT_SECRET`
- `ALLOWED_ORIGINS`
- `CFBD_API_KEY`
- `CFF_ADMIN_API_TOKEN`
- `RESEND_API_KEY`
- `CFF_EMAIL_FROM`
- `CFF_FRONTEND_BASE_URL`

Backup cron:

- `CFF_BACKUP_S3_BUCKET`
- `AWS_ACCESS_KEY_ID`
- `AWS_SECRET_ACCESS_KEY`
- `AWS_REGION`

Never reuse `JWT_SECRET` as the admin token, email credential, or backup credential.

## Release sequence

1. Sync the Blueprint and deploy the latest `main` commit.
2. Run **Render staging validation** with email checks enabled.
3. Trigger the weekly player ingestion manually and verify active player counts.
4. Trigger live ingestion and verify schedule/live cache population.
5. Trigger the backup cron and confirm encrypted dump plus SHA-256 checksum in off-platform storage.
6. Restore that backup into a disposable database and validate the restored records.
7. Run **Alpha release readiness** with:
   - `require_email=true`
   - `backup_restore_verified=true`
   - `run_browser_suite=true`
8. Download and retain the Alpha readiness and browser evidence artifact.
9. Complete one full test draft and one scoring week with the pilot users.
10. Record defects and block Alpha promotion for every critical or high-severity issue.

## Email acceptance

The Alpha workflow requires email delivery configuration and runs the existing verification and password-recovery acceptance checks. Do not enable required verification for public users until the sender is approved and both flows pass against the deployed site.

## Promotion rule

The application may be labeled **Alpha** only when:

- the automated Alpha workflow passes;
- one complete draft and scoring week pass;
- the real off-platform restore has been verified;
- no critical or high-severity defects remain.

The controlled Alpha is limited to one or two known test leagues with resettable data.
