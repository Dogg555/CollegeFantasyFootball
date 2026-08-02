CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- Invalidate old raw tokens once; all newly issued values are stored as digests.
DELETE FROM auth_tokens;
UPDATE users SET email_verification_token = NULL, email_verification_expires_at = NULL,
                 password_reset_token = NULL, password_reset_expires_at = NULL,
                 updated_at = NOW();

DO $$
BEGIN
  IF EXISTS (SELECT lower(btrim(email)) FROM users GROUP BY lower(btrim(email)) HAVING COUNT(*) > 1) THEN
    RAISE EXCEPTION 'Cannot normalize account emails because case-insensitive duplicates exist';
  END IF;
END
$$;

ALTER TABLE auth_tokens DROP CONSTRAINT IF EXISTS auth_tokens_email_fkey;
UPDATE users SET email = lower(btrim(email));
UPDATE leagues SET account_email = lower(btrim(account_email)),
  invited_emails = ARRAY(SELECT DISTINCT lower(btrim(value)) FROM unnest(invited_emails) AS value WHERE btrim(value) <> '');
UPDATE league_members SET email = lower(btrim(email)),
  invited_by_email = CASE WHEN invited_by_email IS NULL THEN NULL ELSE lower(btrim(invited_by_email)) END;
UPDATE rosters SET manager_email = lower(btrim(manager_email));
UPDATE draft_states SET draft_order = ARRAY(SELECT lower(btrim(value)) FROM unnest(draft_order) AS value);
UPDATE draft_queues SET manager_email = lower(btrim(manager_email));
UPDATE draft_picks SET manager_email = lower(btrim(manager_email));
UPDATE waiver_claims SET manager_email = lower(btrim(manager_email));
UPDATE waiver_priorities SET manager_email = lower(btrim(manager_email));
UPDATE trade_offers SET offered_by_email = lower(btrim(offered_by_email)), offered_to_email = lower(btrim(offered_to_email));
UPDATE transactions SET manager_email = CASE WHEN manager_email IS NULL THEN NULL ELSE lower(btrim(manager_email)) END;
UPDATE league_matchups SET home_manager_email = lower(btrim(home_manager_email)),
  away_manager_email = CASE WHEN away_manager_email IS NULL THEN NULL ELSE lower(btrim(away_manager_email)) END;
UPDATE fantasy_player_scores SET manager_email = lower(btrim(manager_email));
ALTER TABLE auth_tokens ADD CONSTRAINT auth_tokens_email_fkey
  FOREIGN KEY (email) REFERENCES users(email) ON DELETE CASCADE ON UPDATE CASCADE;
CREATE UNIQUE INDEX IF NOT EXISTS ux_users_email_normalized ON users (lower(btrim(email)));
ALTER TABLE users DROP CONSTRAINT IF EXISTS users_email_canonical;
ALTER TABLE users ADD CONSTRAINT users_email_canonical CHECK (email = lower(btrim(email)));
ALTER TABLE league_members DROP CONSTRAINT IF EXISTS league_members_email_canonical;
ALTER TABLE league_members ADD CONSTRAINT league_members_email_canonical CHECK (email = lower(btrim(email)));
