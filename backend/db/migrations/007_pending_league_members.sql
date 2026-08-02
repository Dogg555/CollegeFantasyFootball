ALTER TABLE league_members DROP CONSTRAINT IF EXISTS league_members_status_check;
ALTER TABLE league_members ADD CONSTRAINT league_members_status_check
  CHECK (status IN ('invited', 'pending', 'active', 'removed'));
