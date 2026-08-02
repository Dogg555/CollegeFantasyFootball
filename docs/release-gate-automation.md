# Release-gate automation

The application remains pre-alpha until all four release gates produce retained evidence against the deployed staging environment.

## 1. Transactional email acceptance

Run `scripts/email_acceptance.py` with a real inbox that supports plus aliases and IMAP.

Required variables:

- `CFF_API_BASE_URL`
- `CFF_FRONTEND_BASE_URL`
- `CFF_EMAIL_TEST_ADDRESS`
- `CFF_IMAP_HOST`
- `CFF_IMAP_USERNAME` (defaults to the test address)
- `CFF_IMAP_PASSWORD`

Optional variables:

- `CFF_IMAP_PORT=993`
- `CFF_IMAP_FOLDER=INBOX`
- `CFF_EMAIL_WAIT_SECONDS=240`
- `CFF_EMAIL_EXPECTED_FROM_DOMAIN`
- `CFF_EMAIL_ACCEPTANCE_PASSWORD`

The gate creates a unique plus-alias account, proves the initial verification message arrives, requests a replacement verification message, rejects the superseded token, activates the account, completes password recovery, verifies session revocation and single-use reset tokens, and compares public responses for existing and nonexistent accounts.

No raw verification, reset, or session token is written to the evidence artifact.

## 2. Data-ingestion validation

Run `scripts/data_ingestion_validation.py` after the weekly roster job and at least one live/schedule job.

Required variables:

- `CFF_API_BASE_URL`
- `CFF_ADMIN_API_TOKEN`

Useful thresholds:

- `CFF_DATA_EXPECTED_SEASON`
- `CFF_DATA_MIN_ACTIVE_PLAYERS=1000`
- `CFF_DATA_MIN_TEAMS=100`
- `CFF_DATA_MIN_CONFERENCES=8`
- `CFF_DATA_MAX_MISSING_PERCENT=2`
- `CFF_DATA_MAX_MONTHLY_CFBD_CALLS=125000`
- `CFF_DATA_REQUIRE_SCHEDULE=true`

Use `--run-player-ingest` or `--run-live-ingest` for an explicit one-off refresh before validation. The default Alpha workflow validates existing scheduled results to avoid unintentionally consuming the CFBD allowance on every run.

The report includes the latest player-ingestion ledger entry, catalog metadata, current-season sample quality, duplicate detection across pages, search/filter checks, schedule shape, live-ingestion status, and monthly API calls.

## 3. Full fantasy lifecycle

Run `scripts/full_lifecycle_tests.py` only against staging or another disposable environment. Three distinct, preverified accounts are required:

- `CFF_LIFECYCLE_COMMISSIONER_EMAIL`
- `CFF_LIFECYCLE_COMMISSIONER_PASSWORD`
- `CFF_LIFECYCLE_MANAGER_A_EMAIL`
- `CFF_LIFECYCLE_MANAGER_A_PASSWORD`
- `CFF_LIFECYCLE_MANAGER_B_EMAIL`
- `CFF_LIFECYCLE_MANAGER_B_PASSWORD`

The test creates a release-gate league and a separate isolation league, then exercises:

- login and session creation;
- cross-league access denial;
- invitations, join requests, commissioner approval, and persisted membership;
- commissioner-only settings and draft controls;
- draft order, queue persistence, three snake picks, and complete one-player rosters;
- declined and accepted trades with roster swaps;
- cancelled and processed waivers;
- free-agent drop/add behavior;
- one-week schedule generation, scoring, repeatable finalization, derived standings evidence, and transaction history.

Created leagues are deleted in a `finally` block unless `CFF_LIFECYCLE_KEEP_RESOURCES=true`.

## 4. Backup creation and restore

The daily backup container now performs four checks before reporting success:

1. `pg_dump` creates a custom-format archive above `CFF_BACKUP_MIN_BYTES`.
2. `pg_restore --list` confirms the local archive is readable.
3. The uploaded object, metadata checksum, and sidecar checksum match.
4. When `CFF_BACKUP_VERIFY_UPLOAD=true`, the object is downloaded again, rehashed, and inspected with `pg_restore --list`.

Use the manual **Backup restore validation** GitHub Actions workflow for a real recovery drill. It requires an empty or explicitly disposable PostgreSQL database and off-platform object-store credentials.

Mandatory safety variables:

- `CFF_RESTORE_TARGET_DB_URL`
- `CFF_RESTORE_CONFIRM=restore-disposable-database`
- `CFF_BACKUP_S3_BUCKET`
- `AWS_ACCESS_KEY_ID`
- `AWS_SECRET_ACCESS_KEY`

The restore tool refuses to use the same database identity as `DB_URL`, refuses a nonempty target unless `CFF_RESTORE_ALLOW_NONEMPTY=true`, verifies the checksum and archive, restores with `--exit-on-error`, checks core table counts and league foreign-key integrity, and emits a SHA-256 evidence file.

## Evidence and Alpha promotion

All tools write JSON and Markdown under `release-gate-artifacts/`. The Alpha workflow uploads that directory even when a gate fails.

A passing Alpha run requires:

- transactional email acceptance when `require_email=true`;
- current-season data validation;
- the three-user lifecycle when `run_full_lifecycle=true`;
- the successful Backup restore validation run ID and its 64-character evidence SHA-256;
- existing API, security, CORS, public metadata, and browser checks.

Retain the workflow artifact and the backup-restore evidence artifact with the release decision.
