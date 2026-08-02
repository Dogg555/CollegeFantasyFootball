# Alpha release runbook

College Fantasy Football remains **pre-alpha** until the deployed Alpha workflow passes and the manual pilot evidence is recorded.

## Repository and Render configuration

The Render Blueprint defines:

- `college-ff-api`
- `college-ff-frontend`
- weekly `college-ff-cfbd-ingest`
- seasonal two-minute `college-ff-live-ingest`
- daily encrypted and re-verified `college-ff-db-backup`
- PostgreSQL `college-ff-db`

The full roster sync runs weekly at `0 10 * * 2` UTC. Use Render **Trigger Run** for preseason, transfer, or emergency refreshes. Live score polling remains separate.

## Required GitHub Actions secrets

Base staging validation:

- `CFF_STAGING_API_BASE_URL`
- `CFF_STAGING_FRONTEND_BASE_URL`
- `CFF_STAGING_ADMIN_API_TOKEN`
- `CFF_STAGING_SMOKE_EMAIL`
- `CFF_STAGING_SMOKE_PASSWORD`
- `CFF_STAGING_VERIFICATION_EMAIL_BASE`

Transactional email acceptance:

- `CFF_STAGING_EMAIL_TEST_ADDRESS`
- `CFF_STAGING_EMAIL_ACCEPTANCE_PASSWORD`
- `CFF_STAGING_EMAIL_FROM_DOMAIN`
- `CFF_STAGING_IMAP_HOST`
- `CFF_STAGING_IMAP_PORT`
- `CFF_STAGING_IMAP_USERNAME`
- `CFF_STAGING_IMAP_PASSWORD`

Three-user lifecycle:

- `CFF_STAGING_LIFECYCLE_COMMISSIONER_EMAIL`
- `CFF_STAGING_LIFECYCLE_COMMISSIONER_PASSWORD`
- `CFF_STAGING_LIFECYCLE_MANAGER_A_EMAIL`
- `CFF_STAGING_LIFECYCLE_MANAGER_A_PASSWORD`
- `CFF_STAGING_LIFECYCLE_MANAGER_B_EMAIL`
- `CFF_STAGING_LIFECYCLE_MANAGER_B_PASSWORD`

Backup restore workflow:

- `CFF_RESTORE_TARGET_DB_URL`
- `CFF_BACKUP_S3_BUCKET`
- `CFF_BACKUP_AWS_ACCESS_KEY_ID`
- `CFF_BACKUP_AWS_SECRET_ACCESS_KEY`
- `CFF_BACKUP_AWS_ENDPOINT_URL` when required by the object store

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
- `AWS_ENDPOINT_URL` when required
- `CFF_BACKUP_VERIFY_UPLOAD=true`

Never reuse `JWT_SECRET` as the admin token, email credential, or backup credential.

## Release sequence

1. Sync the Blueprint and deploy the latest `main` commit.
2. Configure the verified transactional-email sender and set `CFF_REQUIRE_EMAIL_VERIFICATION=true` in staging.
3. Run **Render staging validation**.
4. Trigger the weekly player ingestion manually and verify active player counts.
5. Trigger live ingestion and verify schedule/live cache population.
6. Run **Backup restore validation** against an empty disposable PostgreSQL 16 database.
7. Download and retain the restore evidence artifact. Copy its workflow run ID and 64-character evidence SHA-256 value.
8. Run **Alpha release readiness** with:
   - `require_email=true`
   - `backup_restore_run_id=<workflow run id>`
   - `backup_restore_evidence_sha256=<restore evidence digest>`
   - `run_full_lifecycle=true`
   - `run_browser_suite=true`
9. Download and retain the Alpha readiness, release-gate, and browser evidence artifacts.
10. Review the generated lifecycle evidence and complete any remaining manual mobile/desktop observations.
11. Record defects and block Alpha promotion for every critical or high-severity issue.

## Automated acceptance gates

The Alpha workflow now proves:

- verification and password-reset messages arrive in a real IMAP inbox;
- replacement and single-use recovery tokens behave correctly;
- the current-season player catalog and schedule meet quality thresholds;
- three preverified users can complete membership, draft, waiver, trade, scoring, and finalization flows;
- cross-league access is denied;
- a real off-platform backup restore produced retained cryptographic evidence.

See `docs/release-gate-automation.md` for variables, safety controls, and evidence formats.

## Promotion rule

The application may be labeled **Alpha** only when:

- the automated Alpha workflow passes;
- the real off-platform restore has been verified and its evidence digest supplied;
- no critical or high-severity defects remain;
- the evidence artifacts are retained with the release decision.

The controlled Alpha is limited to one or two known test leagues with resettable data.
