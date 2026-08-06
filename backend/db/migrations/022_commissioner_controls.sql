-- Task 4: safe commissioner controls and competition-setting locks.

CREATE TABLE IF NOT EXISTS commissioner_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_commissioner_operations_created
  ON commissioner_operations (league_id, created_at DESC);

CREATE OR REPLACE FUNCTION enforce_league_competition_settings_lock()
RETURNS TRIGGER AS $$
DECLARE
  competition_started BOOLEAN;
  active_manager_count INTEGER;
BEGIN
  SELECT COUNT(*)
  INTO active_manager_count
  FROM league_members
  WHERE league_id = OLD.id
    AND status = 'active';

  IF NEW.team_count < active_manager_count THEN
    RAISE EXCEPTION 'team_count cannot be lower than the active manager count'
      USING ERRCODE = '23514';
  END IF;

  SELECT (
    EXISTS (
      SELECT 1 FROM draft_states
      WHERE league_id = OLD.id AND status <> 'not_started'
    )
    OR EXISTS (
      SELECT 1 FROM draft_picks WHERE league_id = OLD.id
    )
  ) INTO competition_started;

  IF competition_started AND (
    NEW.team_count IS DISTINCT FROM OLD.team_count
    OR NEW.scoring IS DISTINCT FROM OLD.scoring
    OR NEW.scoring_settings IS DISTINCT FROM OLD.scoring_settings
    OR NEW.draft_type IS DISTINCT FROM OLD.draft_type
    OR NEW.draft_date IS DISTINCT FROM OLD.draft_date
    OR NEW.roster_rules IS DISTINCT FROM OLD.roster_rules
  ) THEN
    RAISE EXCEPTION 'core league settings are locked after the draft starts'
      USING ERRCODE = '55000';
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_league_competition_settings_lock ON leagues;
CREATE TRIGGER trg_league_competition_settings_lock
BEFORE UPDATE ON leagues
FOR EACH ROW
EXECUTE FUNCTION enforce_league_competition_settings_lock();

CREATE OR REPLACE FUNCTION enforce_league_member_transition()
RETURNS TRIGGER AS $$
DECLARE
  owner_email TEXT;
  configured_teams INTEGER;
  competition_started BOOLEAN;
  reserved_count INTEGER;
  has_competition_data BOOLEAN;
BEGIN
  SELECT account_email, team_count,
    (EXISTS (SELECT 1 FROM draft_states WHERE league_id = NEW.league_id AND status <> 'not_started')
     OR EXISTS (SELECT 1 FROM draft_picks WHERE league_id = NEW.league_id))
  INTO owner_email, configured_teams, competition_started
  FROM leagues
  WHERE id = NEW.league_id;

  IF lower(NEW.email) = lower(owner_email)
     AND (NEW.role <> 'commissioner' OR NEW.status <> 'active') THEN
    RAISE EXCEPTION 'the league owner must remain an active commissioner'
      USING ERRCODE = '42501';
  END IF;

  IF TG_OP = 'INSERT' THEN
    IF competition_started AND NEW.status <> 'removed' THEN
      RAISE EXCEPTION 'managers cannot be added after the draft starts'
        USING ERRCODE = '55000';
    END IF;
    IF NEW.status <> 'removed' THEN
      SELECT COUNT(*) INTO reserved_count
      FROM league_members
      WHERE league_id = NEW.league_id AND status <> 'removed';
      IF reserved_count >= configured_teams THEN
        RAISE EXCEPTION 'the league has no open manager slots'
          USING ERRCODE = '23514';
      END IF;
    END IF;
  ELSIF TG_OP = 'UPDATE' THEN
    IF competition_started AND (
      (OLD.status = 'removed' AND NEW.status <> 'removed')
      OR (OLD.status IN ('invited', 'pending') AND NEW.status = 'active')
    ) THEN
      RAISE EXCEPTION 'managers cannot be added after the draft starts'
        USING ERRCODE = '55000';
    END IF;
    IF OLD.status = 'removed' AND NEW.status <> 'removed' THEN
      SELECT COUNT(*) INTO reserved_count
      FROM league_members
      WHERE league_id = NEW.league_id AND status <> 'removed';
      IF reserved_count >= configured_teams THEN
        RAISE EXCEPTION 'the league has no open manager slots'
          USING ERRCODE = '23514';
      END IF;
    END IF;
  END IF;

  IF TG_OP = 'UPDATE' AND OLD.status = 'active' AND NEW.status = 'removed' THEN
    SELECT (
      EXISTS (SELECT 1 FROM rosters WHERE league_id = NEW.league_id AND lower(manager_email) = lower(NEW.email))
      OR EXISTS (SELECT 1 FROM draft_picks WHERE league_id = NEW.league_id AND lower(manager_email) = lower(NEW.email))
      OR EXISTS (SELECT 1 FROM league_matchups WHERE league_id = NEW.league_id
                 AND (lower(home_manager_email) = lower(NEW.email) OR lower(away_manager_email) = lower(NEW.email)))
      OR EXISTS (SELECT 1 FROM trade_offers WHERE league_id = NEW.league_id
                 AND status IN ('pending', 'accepted')
                 AND (lower(offered_by_email) = lower(NEW.email) OR lower(offered_to_email) = lower(NEW.email)))
      OR EXISTS (SELECT 1 FROM waiver_claims WHERE league_id = NEW.league_id
                 AND status = 'pending' AND lower(manager_email) = lower(NEW.email))
    ) INTO has_competition_data;

    IF competition_started OR has_competition_data THEN
      RAISE EXCEPTION 'resolve manager competition data before removal'
        USING ERRCODE = '55000';
    END IF;
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_league_member_transition ON league_members;
CREATE TRIGGER trg_league_member_transition
BEFORE INSERT OR UPDATE OF status, role ON league_members
FOR EACH ROW
EXECUTE FUNCTION enforce_league_member_transition();

CREATE OR REPLACE FUNCTION cleanup_removed_league_member()
RETURNS TRIGGER AS $$
BEGIN
  IF OLD.status <> 'removed' AND NEW.status = 'removed' THEN
    DELETE FROM draft_queues
      WHERE league_id = NEW.league_id AND lower(manager_email) = lower(NEW.email);
    DELETE FROM draft_readiness
      WHERE league_id = NEW.league_id AND lower(manager_email) = lower(NEW.email);
    DELETE FROM waiver_priorities
      WHERE league_id = NEW.league_id AND lower(manager_email) = lower(NEW.email);
    UPDATE draft_states
      SET draft_order = array_remove(draft_order, NEW.email), updated_at = NOW()
      WHERE league_id = NEW.league_id AND status = 'not_started';
    UPDATE leagues
      SET invited_emails = array_remove(invited_emails, NEW.email), updated_at = NOW()
      WHERE id = NEW.league_id;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_cleanup_removed_league_member ON league_members;
CREATE TRIGGER trg_cleanup_removed_league_member
AFTER UPDATE OF status ON league_members
FOR EACH ROW
EXECUTE FUNCTION cleanup_removed_league_member();
