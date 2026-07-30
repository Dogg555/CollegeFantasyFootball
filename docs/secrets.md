# Secret Handling

Never commit real secrets, API keys, TLS private keys, database dumps, or `.env.local`.

## If a Secret Is Committed
1. Rotate the secret immediately in the upstream provider.
2. Remove the secret from the current branch.
3. If the repository has already been pushed publicly, treat the secret as compromised.
4. Use a history-rewrite tool only after coordinating with contributors.

Useful tools:
- GitHub secret scanning
- `gitleaks`
- `detect-secrets`

Example scan:
```sh
gitleaks detect --source .
```

Do not rely on history rewriting as a substitute for rotation. Rotation comes first.
