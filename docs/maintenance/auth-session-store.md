# Authentication session store extraction

This maintenance branch moves only session-token issuance, persistence, lookup, expiration cleanup, and revocation out of `backend/src/main.cpp`.

Account creation, password verification, email verification, password reset, HTTP routes, response bodies, status codes, migrations, and deployment configuration remain unchanged.

The branch targets `Test` only and must pass strict unit tests plus the complete production-image authentication contract matrix before merge.
