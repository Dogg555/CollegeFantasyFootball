# Security Policy

## Supported version

Security fixes are applied to the `main` branch. Deployments should track a reviewed commit from `main` and keep all required security workflows green.

## Reporting a vulnerability

Do not open a public issue for suspected vulnerabilities, leaked credentials, authentication bypasses, or private user data.

Use GitHub's **Report a vulnerability** option on the repository's Security page when it is available. Otherwise, contact the repository owner privately through their GitHub profile and include:

- the affected endpoint or component;
- reproducible steps that do not access other users' data;
- the likely impact;
- relevant request IDs, timestamps, or sanitized logs;
- a suggested remediation, when known.

Do not include passwords, bearer tokens, API keys, database URLs, private email addresses, or production data in the report.

## Handling secrets

- Do not commit `.env`, `.env.local`, `.env.*`, `certs/`, `.secrets/`, database backups, or override compose files containing credentials.
- Use GitHub Actions and hosting-provider secrets for CI and production.
- If a credential is exposed, rotate or revoke it before scrubbing repository history.
- Never copy raw bearer tokens, reset tokens, API keys, or database URLs into issues or logs.

## Operational response

For a confirmed credential exposure or authentication issue:

1. Revoke or rotate the affected credential immediately.
2. Revoke active sessions when authentication material may be affected.
3. Review security and application logs without copying raw secrets.
4. Patch and validate the issue in a private branch.
5. Deploy only after CI, CodeQL, Trivy, and secret scanning pass.

## Pre-merge checks

- Require CI, CodeQL, Trivy filesystem and image scanning, and secret scanning.
- Run the API security smoke suite for authentication, CORS, request limits, headers, and admin throttling.
- Run the database backup and restore drill after migration or schema changes.
- Review GitHub secret scanning results before merging public contributions.
