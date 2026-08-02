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

The domain portion of `CFF_EMAIL_FROM` must exactly match a domain or subdomain that is verified in the same Resend account as `RESEND_API_KEY`.

Examples:

- If Resend verifies `college-fantasy-football.com`, use `noreply@college-fantasy-football.com`.
- If Resend verifies `notify.college-fantasy-football.com`, use `noreply@notify.college-fantasy-football.com` rather than the root domain.

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

## Signup reports an API error after email setup

Signup stores the account before attempting verification delivery. If Resend rejects the message, the account may already exist but remain unverified. A second signup attempt then returns `409 Account already exists`.

Recovery steps:

1. Correct the Resend API key, verified sending domain, and `CFF_EMAIL_FROM` value.
2. Redeploy the API.
3. Open `resend-verification.html` and request a new verification message for the existing account.
4. Do not repeatedly submit signup for the same email.

Render logs include a line beginning with `[email] resend delivery failed`. The enhanced logging includes Resend's HTTP status and safe error response so domain mismatch, invalid API key, and testing-address restrictions can be distinguished.
