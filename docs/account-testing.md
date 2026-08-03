# Account signup testing and reset

The `users.email` column is the primary key, so PostgreSQL cannot store two accounts with the same canonical email. Repeating signup for an existing address does not replace its password or create another row. The API intentionally returns a generic response so attackers cannot reliably use signup to enumerate registered users.

## Diagnose an account

Open a Render Shell for the API service and run:

```sh
sh /srv/db/account-admin.sh inspect --email test-user@example.com
```

The command shows whether the account exists, whether it is verified, whether a verification token is present, active session count, and related league/fantasy row counts.

A common apparent signup failure is:

1. The new user row is created.
2. Verification delivery fails or the browser reports a timeout.
3. Retrying with the same address reaches the existing-account path.

Use `inspect` before deleting anything. It distinguishes a database insert failure from an email-delivery or frontend-reporting failure.

## Delete a disposable account

Deletion requires the exact canonical email twice:

```sh
sh /srv/db/account-admin.sh delete \
  --email test-user@example.com \
  --confirm-email test-user@example.com
```

The command refuses to delete an account that owns leagues, belongs to leagues, or has fantasy records. This prevents a routine signup retest from damaging real league data.

For an entirely disposable test account and disposable league data, explicitly add:

```sh
sh /srv/db/account-admin.sh delete \
  --email test-user@example.com \
  --confirm-email test-user@example.com \
  --purge-related
```

`--purge-related` deletes leagues owned by the account and removes its roster, draft, waiver, trade, matchup, scoring, membership, invitation, session, and account records. Do not use it for a real tester without reviewing `inspect` first.

## Retest signup

After deletion:

1. Open a private/incognito browser window.
2. Hard-refresh the signup page.
3. Register the deleted email once.
4. Inspect the account immediately.
5. Check the API logs using the request reference shown by the frontend.
6. Confirm verification delivery and then verify the account.

Do not repeatedly submit the form. Duplicate submissions are throttled and an ambiguous timeout may occur after the database already committed the account.

## Production verification

For verification-required signup, confirm these Render environment variables are configured:

```text
CFF_REQUIRE_EMAIL_VERIFICATION=true
CFF_EMAIL_PROVIDER=resend
RESEND_API_KEY=<secret>
CFF_EMAIL_FROM=<verified sender>
CFF_FRONTEND_BASE_URL=https://college-fantasy-football.com
```

For SMTP, configure the `CFF_SMTP_*` variables instead of `RESEND_API_KEY`. The sender domain must be accepted by the provider.
