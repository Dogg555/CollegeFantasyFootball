CREATE TABLE IF NOT EXISTS trade_states (
  league_id TEXT PRIMARY KEY REFERENCES leagues(id) ON DELETE CASCADE,
  version BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS trade_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  actor_email TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, actor_email, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_trade_operations_created
  ON trade_operations (league_id, actor_email, created_at DESC);

ALTER TABLE trade_offers ADD COLUMN IF NOT EXISTS state_version BIGINT NOT NULL DEFAULT 0;
ALTER TABLE trade_offers ADD COLUMN IF NOT EXISTS resolution_reason TEXT NOT NULL DEFAULT '';
ALTER TABLE trade_offers ADD COLUMN IF NOT EXISTS accepted_at TIMESTAMPTZ;
ALTER TABLE trade_offers ADD COLUMN IF NOT EXISTS approved_at TIMESTAMPTZ;
ALTER TABLE trade_offers ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

UPDATE trade_offers
SET status = 'expired',
    resolution_reason = CASE
      WHEN resolution_reason = '' THEN 'deadline_passed'
      ELSE resolution_reason
    END,
    resolved_at = COALESCE(resolved_at, NOW()),
    updated_at = NOW()
WHERE status IN ('pending', 'accepted')
  AND expires_at IS NOT NULL
  AND expires_at <= NOW();

WITH player_participation AS (
  SELECT id AS offer_id, league_id, offered_by_email AS manager_email,
         unnest(offered_player_ids) AS player_id, created_at
  FROM trade_offers
  WHERE status IN ('pending', 'accepted')
  UNION ALL
  SELECT id AS offer_id, league_id, offered_to_email AS manager_email,
         unnest(requested_player_ids) AS player_id, created_at
  FROM trade_offers
  WHERE status IN ('pending', 'accepted')
), ranked AS (
  SELECT offer_id, league_id, player_id,
         ROW_NUMBER() OVER (
           PARTITION BY league_id, player_id
           ORDER BY created_at ASC, offer_id ASC
         ) AS lock_rank
  FROM player_participation
  WHERE player_id IS NOT NULL AND player_id <> ''
), conflicting_offers AS (
  SELECT DISTINCT offer_id
  FROM ranked
  WHERE lock_rank > 1
)
UPDATE trade_offers
SET status = 'expired',
    resolution_reason = 'legacy_player_lock_conflict',
    resolved_at = COALESCE(resolved_at, NOW()),
    updated_at = NOW()
WHERE id IN (SELECT offer_id FROM conflicting_offers)
  AND status IN ('pending', 'accepted');

CREATE TABLE IF NOT EXISTS trade_player_locks (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  player_id TEXT NOT NULL,
  offer_id TEXT NOT NULL REFERENCES trade_offers(id) ON DELETE CASCADE,
  manager_email TEXT NOT NULL,
  lock_role TEXT NOT NULL CHECK (lock_role IN ('offered', 'requested')),
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, player_id),
  UNIQUE (offer_id, player_id)
);

INSERT INTO trade_player_locks (league_id, player_id, offer_id, manager_email, lock_role)
SELECT offer.league_id, offered.player_id, offer.id, lower(offer.offered_by_email), 'offered'
FROM trade_offers offer
CROSS JOIN LATERAL unnest(offer.offered_player_ids) AS offered(player_id)
WHERE offer.status IN ('pending', 'accepted')
  AND offered.player_id <> ''
ON CONFLICT DO NOTHING;

INSERT INTO trade_player_locks (league_id, player_id, offer_id, manager_email, lock_role)
SELECT offer.league_id, requested.player_id, offer.id, lower(offer.offered_to_email), 'requested'
FROM trade_offers offer
CROSS JOIN LATERAL unnest(offer.requested_player_ids) AS requested(player_id)
WHERE offer.status IN ('pending', 'accepted')
  AND requested.player_id <> ''
ON CONFLICT DO NOTHING;

CREATE INDEX IF NOT EXISTS idx_trade_offers_open_expiration
  ON trade_offers (league_id, expires_at, created_at)
  WHERE status IN ('pending', 'accepted');

CREATE INDEX IF NOT EXISTS idx_trade_offers_participants
  ON trade_offers (league_id, offered_by_email, offered_to_email, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_trade_player_locks_offer
  ON trade_player_locks (offer_id);
