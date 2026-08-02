# Transactional email provider setup

The backend supports two delivery providers through the same signup-verification and password-reset flows.

## Provider selection

Set `CFF_EMAIL_PROVIDER` to one of:

- `resend`
- `smtp`

Both providers also require:

- `CFF_EMAIL_FROM`
- `CFF_FRONTEND_BASE_URL`

`CFF_EMAIL_FROM` may include a display name, for example:

```text
College Fantasy Football <noreply@example.com>
```

## Resend

Set:

```text
CFF_EMAIL_PROVIDER=resend
RESEND_API_KEY=<secret>
CFF_EMAIL_FROM=College Fantasy Football <noreply@example.com>
CFF_FRONTEND_BASE_URL=https://your-frontend.example
```

## SMTP

Set:

```text
CFF_EMAIL_PROVIDER=smtp
CFF_SMTP_HOST=smtp.provider.example
CFF_SMTP_PORT=587
CFF_SMTP_SECURITY=starttls
CFF_SMTP_USERNAME=<secret username>
CFF_SMTP_PASSWORD=<secret password or app password>
CFF_EMAIL_FROM=College Fantasy Football <noreply@example.com>
CFF_FRONTEND_BASE_URL=https://your-frontend.example
```

Supported security modes:

- `starttls`: connect with SMTP and require TLS upgrade; normally port 587
- `tls` or `smtps`: implicit TLS; normally port 465

TLS certificate and hostname verification remain enabled. Do not use providers that require disabling certificate verification.

## Enabling verification

Keep `CFF_REQUIRE_EMAIL_VERIFICATION=false` until all of these pass:

1. A signup verification message is delivered.
2. The verification link points to the deployed frontend.
3. Resend verification works.
4. Password-reset delivery works.
5. The reset link completes successfully.
6. Logs contain no SMTP credentials, API keys, or raw verification/reset tokens.

After testing, set:

```text
CFF_REQUIRE_EMAIL_VERIFICATION=true
```

Then run the **Alpha release readiness** workflow with email verification required.
