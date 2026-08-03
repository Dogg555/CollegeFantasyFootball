# Signup reliability and security

The production signup path follows these rules:

- Local/demo sessions are permitted only when explicitly enabled on localhost.
- Authentication requests use bounded client-side timeouts and are never automatically retried.
- Submit buttons are disabled while an authentication request is active.
- A timed-out signup is treated as ambiguous because the server may have committed the account before the response was lost.
- Failed verification-email delivery does not turn successful account creation into a failed-signup message.
- Duplicate signup requests return an enumeration-safe accepted response rather than confirming that an address is registered.
- Invalid email, missing-password, and weak-password requests do not consume the strict signup allowance.
- A high short-window authentication burst ceiling still protects JSON parsing and request resources from malformed-request floods.
- Valid signup attempts are limited by both apparent client and canonical-email fingerprint.
- Authentication responses may expose a sanitized request reference through `X-CFF-Request-Id`; credentials and raw tokens are never included in that reference or security logs.

## Default controls

| Setting | Default | Purpose |
|---|---:|---|
| `CFF_AUTH_BURST_CLIENT_LIMIT` | 240 | Maximum authentication mutations in the short burst window |
| `CFF_AUTH_BURST_WINDOW_SECONDS` | 60 | Burst-window duration |
| `CFF_SIGNUP_CLIENT_LIMIT` | 120 | Valid signup attempts per apparent client during the signup window |
| `CFF_SIGNUP_ACCOUNT_LIMIT` | 5 | Valid signup attempts for one canonical email during the signup window |
| `CFF_SIGNUP_RATE_WINDOW_MINUTES` | 60 | Strict signup-window duration |

Rate settings should be adjusted only with retained abuse and legitimate-user evidence. The per-email limit is the primary signup-abuse control; the higher client limit avoids locking unrelated users behind a shared proxy or network address.
