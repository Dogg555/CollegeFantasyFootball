-- Task 5: database-level atomicity invariants for fantasy mutations.
--
-- These constraint triggers are deferred until transaction commit. Multi-step
-- mutations may therefore change several tables in any order, but PostgreSQL
-- rejects the entire transaction if the final committed state is incomplete.

CREATE OR REPLACE FUNCTION enforce_atomic_draft_pick_roster()
RETURNS TRIGGER AS $$
BEGIN
  IF NOT EXISTS (
    SELECT 1
    FROM rosters roster
    WHERE roster.league_id = NEW.league_id
      AND lower(roster.manager_email) = lower(NEW.manager_email)
      AND roster.player_id = NEW.player_id
  ) THEN
    RAISE EXCEPTION 'draft pick must commit with matching roster ownership'
      USING ERRCODE = '23514';
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_atomic_draft_pick_roster ON draft_picks;
CREATE CONSTRAINT TRIGGER trg_atomic_draft_pick_roster
AFTER INSERT OR UPDATE OF league_id, manager_email, player_id ON draft_picks
DEFERRABLE INITIALLY DEFERRED
FOR EACH ROW
EXECUTE FUNCTION enforce_atomic_draft_pick_roster();

CREATE OR REPLACE FUNCTION enforce_atomic_processed_waiver()
RETURNS TRIGGER AS $$
DECLARE
  transitioned_to_processed BOOLEAN := FALSE;
BEGIN
  IF TG_OP = 'INSERT' THEN
    transitioned_to_processed := NEW.status = 'processed';
  ELSE
    transitioned_to_processed := NEW.status = 'processed'
      AND OLD.status IS DISTINCT FROM NEW.status;
  END IF;

  IF NOT transitioned_to_processed THEN
    RETURN NEW;
  END IF;

  IF NOT EXISTS (
    SELECT 1
    FROM rosters roster
    WHERE roster.league_id = NEW.league_id
      AND lower(roster.manager_email) = lower(NEW.manager_email)
      AND roster.player_id = NEW.add_player_id
  ) THEN
    RAISE EXCEPTION 'processed waiver must commit with claimed player on manager roster'
      USING ERRCODE = '23514';
  END IF;

  IF COALESCE(NEW.drop_player_id, '') <> '' AND EXISTS (
    SELECT 1
    FROM rosters roster
    WHERE roster.league_id = NEW.league_id
      AND lower(roster.manager_email) = lower(NEW.manager_email)
      AND roster.player_id = NEW.drop_player_id
  ) THEN
    RAISE EXCEPTION 'processed waiver must commit with selected drop removed'
      USING ERRCODE = '23514';
  END IF;

  IF NEW.processed_at IS NULL THEN
    RAISE EXCEPTION 'processed waiver must include a processing timestamp'
      USING ERRCODE = '23514';
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_atomic_processed_waiver ON waiver_claims;
CREATE CONSTRAINT TRIGGER trg_atomic_processed_waiver
AFTER INSERT OR UPDATE OF status, manager_email, add_player_id, drop_player_id ON waiver_claims
DEFERRABLE INITIALLY DEFERRED
FOR EACH ROW
EXECUTE FUNCTION enforce_atomic_processed_waiver();

CREATE OR REPLACE FUNCTION enforce_atomic_approved_trade()
RETURNS TRIGGER AS $$
DECLARE
  transitioned_to_approved BOOLEAN := FALSE;
  player TEXT;
BEGIN
  IF TG_OP = 'INSERT' THEN
    transitioned_to_approved := NEW.status = 'approved';
  ELSE
    transitioned_to_approved := NEW.status = 'approved'
      AND OLD.status IS DISTINCT FROM NEW.status;
  END IF;

  IF NOT transitioned_to_approved THEN
    RETURN NEW;
  END IF;

  IF COALESCE(cardinality(NEW.offered_player_ids), 0) = 0
     OR COALESCE(cardinality(NEW.requested_player_ids), 0) = 0 THEN
    RAISE EXCEPTION 'approved trade must contain players on both sides'
      USING ERRCODE = '23514';
  END IF;

  FOREACH player IN ARRAY NEW.offered_player_ids LOOP
    IF NOT EXISTS (
      SELECT 1
      FROM rosters roster
      WHERE roster.league_id = NEW.league_id
        AND lower(roster.manager_email) = lower(NEW.offered_to_email)
        AND roster.player_id = player
    ) THEN
      RAISE EXCEPTION 'approved trade must commit all offered players to recipient roster'
        USING ERRCODE = '23514';
    END IF;
  END LOOP;

  FOREACH player IN ARRAY NEW.requested_player_ids LOOP
    IF NOT EXISTS (
      SELECT 1
      FROM rosters roster
      WHERE roster.league_id = NEW.league_id
        AND lower(roster.manager_email) = lower(NEW.offered_by_email)
        AND roster.player_id = player
    ) THEN
      RAISE EXCEPTION 'approved trade must commit all requested players to offerer roster'
        USING ERRCODE = '23514';
    END IF;
  END LOOP;

  IF EXISTS (
    SELECT 1
    FROM trade_player_locks lock
    WHERE lock.league_id = NEW.league_id
      AND lock.offer_id = NEW.id
  ) THEN
    RAISE EXCEPTION 'approved trade must release all player locks in the same transaction'
      USING ERRCODE = '23514';
  END IF;

  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_atomic_approved_trade ON trade_offers;
CREATE CONSTRAINT TRIGGER trg_atomic_approved_trade
AFTER INSERT OR UPDATE OF status, offered_by_email, offered_to_email,
  offered_player_ids, requested_player_ids ON trade_offers
DEFERRABLE INITIALLY DEFERRED
FOR EACH ROW
EXECUTE FUNCTION enforce_atomic_approved_trade();
