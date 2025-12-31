# Security Policy

## Reporting a vulnerability
- Please email the maintainers with a clear description, steps to reproduce, and any proof-of-concept details.
- Avoid filing public issues for sensitive reports; request a security review instead.
- We will acknowledge reports within 5 business days and provide an expected timeline for remediation.

## Handling secrets
- Do not commit `.env`, `.env.local`, `.env.*`, `certs/`, `.secrets/`, or override compose files containing credentials.
- Use GitHub Actions secrets for CI and rotate credentials immediately if a leak is suspected.
- If a secret is committed, rotate it first, then scrub it from history (see `docs/secrets.md` for commands).

## Pre-merge checks
- Run `pre-commit run --all-files` to execute `detect-secrets` and `gitleaks`.
- Run the "Secret scanning" GitHub Action results before merging to ensure no secrets are introduced.
