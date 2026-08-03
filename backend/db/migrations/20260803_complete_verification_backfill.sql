-- Complete the production incident backfill for accounts created before the
-- transactional email flow was confirmed.
--
-- The first incident migration used the configuration commit timestamp, but
-- verification was enabled in production later. Accounts created during that
-- gap were never given a working verification path and can still be locked out.
--
-- This cutoff represents the end of the known incident window. Accounts created
-- after it continue to require normal email verification.
UPDATE users
SET email_verified = TRUE,
    email_verification_token = NULL,
    email_verification_expires_at = NULL,
    updated_at = NOW()
WHERE email_verified = FALSE
  AND created_at < TIMESTAMPTZ '2026-08-03 15:48:00+00';
