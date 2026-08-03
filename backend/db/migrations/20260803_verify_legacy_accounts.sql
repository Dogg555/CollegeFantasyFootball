-- Restore access for accounts that existed before email-verification enforcement.
--
-- CFF_REQUIRE_EMAIL_VERIFICATION changed from an explicit false value to a
-- persistent Render setting in commit 513df2cca7b0f5ba9aba4c887035e8eed3c5bcd3
-- at 2026-08-02 16:26:22 UTC. Migration 002 added email_verified with a
-- DEFAULT FALSE, which caused every existing account to be treated as
-- unverified even though those users were never issued verification links.
--
-- Keep accounts created after enforcement unchanged so the verification
-- requirement remains effective for new registrations.
UPDATE users
SET email_verified = TRUE,
    email_verification_token = NULL,
    email_verification_expires_at = NULL,
    updated_at = NOW()
WHERE email_verified = FALSE
  AND created_at < TIMESTAMPTZ '2026-08-02 16:26:22+00';
