UPDATE league_matchups
SET identity_key = id
WHERE identity_key IS NULL OR BTRIM(identity_key) = '';

CREATE OR REPLACE FUNCTION cff_assign_matchup_identity_key()
RETURNS TRIGGER AS $$
BEGIN
  IF NEW.identity_key IS NULL OR BTRIM(NEW.identity_key) = '' THEN
    NEW.identity_key := NEW.id;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_cff_assign_matchup_identity_key ON league_matchups;
CREATE TRIGGER trg_cff_assign_matchup_identity_key
BEFORE INSERT OR UPDATE OF id, identity_key ON league_matchups
FOR EACH ROW
EXECUTE FUNCTION cff_assign_matchup_identity_key();

COMMENT ON FUNCTION cff_assign_matchup_identity_key() IS
  'Keeps the migration-018 unique matchup identity compatible with schedule and scoring writers that use the stable matchup ID as the canonical identity.';
