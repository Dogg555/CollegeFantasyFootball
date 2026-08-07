BEGIN;

ALTER TABLE waiver_claims
    ADD COLUMN IF NOT EXISTS processing_period TEXT NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS failure_reason TEXT NOT NULL DEFAULT '';

ALTER TABLE waiver_claims DROP CONSTRAINT IF EXISTS waiver_claims_status_check;
ALTER TABLE waiver_claims
    ADD CONSTRAINT waiver_claims_status_check
    CHECK (status IN ('pending', 'processed', 'cancelled', 'failed', 'expired'));

CREATE INDEX IF NOT EXISTS idx_waiver_claims_processing_period
    ON waiver_claims (league_id, processing_period, status, priority, claim_order, created_at, id);

ALTER TABLE waiver_states
    ADD COLUMN IF NOT EXISTS current_processing_period TEXT NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS last_processing_period TEXT NOT NULL DEFAULT '';

COMMIT;
