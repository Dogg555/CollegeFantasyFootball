# Backend security hardening

This document describes the controls required before external beta access.

## Required production settings

The API refuses production traffic when `CFF_SECURITY_ENFORCE_PRODUCTION=true` and an unsafe setting is detected.

| Variable | Production value |
|---|---|
| `CFF_SECURITY_ENFORCE_PRODUCTION` | `true` |
| `CFF_ALLOW_SHARED_SECRET_AUTH` | `false` |
| `CFF_EXPOSE_AUTH_TOKENS` | `false` |
| `CFF_LOG_AUTH_TOKENS` | `false` |
| `CFF_MIN_PASSWORD_LENGTH` | `12` or greater |
| `CFF_MAX_REQUEST_BODY_BYTES` | `262144` |
| `CFF_AUTH_REQUEST_BODY_BYTES` | `8192` |
| `CFF_TRUST_PROXY_HEADERS` | `true` only behind a trusted proxy that appends or replaces the configured header |
| `CFF_TRUSTED_CLIENT_IP_HEADER` | `x-forwarded-for` on the current Render deployment |
| `ALLOWED_ORIGINS` | exact HTTPS frontend origins, comma-separated; never `*` |
| `JWT_SECRET` | independently generated value of at least 32 characters |
| `CFF_ADMIN_API_TOKEN` | independently generated value of at least 32 characters |

## Implemented controls

- Global request body, in-memory body, URI, and JSON parser-depth limits.
- JSON content-type enforcement for mutation requests with bodies.
- Exact CORS-origin rejection before routing.
- Authentication and admin rate limits keyed by a non-reversible client fingerprint; account-oriented auth limits also include a hashed email identifier.
- Twelve-character minimum password policy, 72-byte bcrypt maximum, common-password rejection, and bcrypt cost 12.
- Server-side 24-hour sessions, logout revocation, and full session revocation after password reset.
- Admin endpoints protected by an independent operations token or an explicitly allowlisted authenticated email.
- Security response headers, disabled server banner, and no-store caching for API responses.
- Minimal public health output without secret/configuration flags.
- Security logs containing normalized route names, response status, and hashed client identifiers only.
- Transactional, advisory-locked database migrations.
- Permission-restricted custom-format backups with SHA-256 checksums.
- Restore drills restricted to disposable database names and executed with one transaction.

## Backup procedure

```sh
DB_URL='postgresql://...' \
BACKUP_DIR='./backups' \
sh scripts/db-backup.sh
```

Move completed backups to encrypted storage with access controls and retention policies. Do not commit backup files or checksums to Git.

## Restore drill

Create a disposable database whose name ends with `_restore_test`, then run:

```sh
BACKUP_FILE='./backups/college-ff-YYYYMMDDTHHMMSSZ.dump' \
RESTORE_DB_URL='postgresql://.../college_ff_restore_test' \
CFF_ALLOW_DESTRUCTIVE_RESTORE_TEST=true \
sh scripts/db-restore-test.sh
```

The restore script refuses other database names, verifies the checksum when present, restores in a single transaction, and checks required tables.

## Release gate

Before external beta access, require:

1. CI and API smoke tests pass.
2. Security hardening and database recovery jobs pass.
3. CodeQL, Trivy filesystem/image scanning, and secret scanning pass.
4. Production origins and secrets are reviewed in the hosting dashboard.
5. A backup exists and a recent restore drill has succeeded.
