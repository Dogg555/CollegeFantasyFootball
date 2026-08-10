ALTER TABLE draft_states
  ADD COLUMN IF NOT EXISTS paused_remaining_seconds INTEGER;

ALTER TABLE leagues
  ADD COLUMN IF NOT EXISTS draft_timezone TEXT NOT NULL DEFAULT 'UTC';

ALTER TABLE draft_states
  DROP CONSTRAINT IF EXISTS draft_states_status_check;

ALTER TABLE draft_states
  ADD CONSTRAINT draft_states_status_check
  CHECK (status IN ('not_started', 'open', 'paused', 'complete', 'cancelled'));

ALTER TABLE draft_states
  DROP CONSTRAINT IF EXISTS draft_states_paused_remaining_seconds_check;

ALTER TABLE draft_states
  ADD CONSTRAINT draft_states_paused_remaining_seconds_check
  CHECK (paused_remaining_seconds IS NULL OR paused_remaining_seconds >= 0);
