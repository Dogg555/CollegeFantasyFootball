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
- Run available secret scanning before merging, such as `gitleaks detect --source .`.
- If you use pre-commit locally, wire it to `detect-secrets` or `gitleaks` and run `pre-commit run --all-files`.
- Review GitHub secret scanning results before merging public contributions.
