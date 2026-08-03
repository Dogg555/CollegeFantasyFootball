#!/bin/sh
set -eu

usage() {
  cat <<'EOF'
Usage:
  account-admin.sh inspect --email user@example.com
  account-admin.sh delete --email user@example.com --confirm-email user@example.com [--purge-related]

Commands:
  inspect          Show account, verification, session, league, and fantasy-data counts.
  delete           Delete one account after exact email confirmation.

Safety:
  delete refuses accounts with related league or fantasy data unless --purge-related is supplied.
  --purge-related also removes owned leagues and the account's records from leagues owned by others.
EOF
}

[ "$#" -ge 1 ] || { usage; exit 2; }
command_name="$1"
shift
email=""
confirm_email=""
purge_related=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --email)
      [ "$#" -ge 2 ] || { echo "--email requires a value" >&2; exit 2; }
      email="$2"
      shift 2
      ;;
    --confirm-email)
      [ "$#" -ge 2 ] || { echo "--confirm-email requires a value" >&2; exit 2; }
      confirm_email="$2"
      shift 2
      ;;
    --purge-related)
      purge_related=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[ -n "${DB_URL:-}" ] || { echo "DB_URL is required" >&2; exit 2; }
[ -n "$email" ] || { echo "--email is required" >&2; exit 2; }

email="$(printf '%s' "$email" | tr '[:upper:]' '[:lower:]' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
confirm_email="$(printf '%s' "$confirm_email" | tr '[:upper:]' '[:lower:]' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

case "$email" in
  *[!A-Za-z0-9@._+\'-]*|*@*@*|@*|*@|*..*|"")
    echo "Refusing invalid email value" >&2
    exit 2
    ;;
esac

psql_base() {
  psql "$DB_URL" -X -q -v ON_ERROR_STOP=1 -v email="$email" "$@"
}

inspect_account() {
  psql_base <<'SQL'
\pset pager off
\echo 'Account state'
SELECT
  email,
  email_verified,
  email_verification_token IS NOT NULL AS verification_token_present,
  email_verification_expires_at,
  password_reset_token IS NOT NULL AS reset_token_present,
  created_at,
  updated_at
FROM users
WHERE email = :'email';

\echo 'Related row counts'
SELECT
  (SELECT COUNT(*) FROM auth_tokens WHERE email = :'email') AS sessions,
  (SELECT COUNT(*) FROM leagues WHERE account_email = :'email') AS owned_leagues,
  (SELECT COUNT(*) FROM league_members WHERE email = :'email') AS memberships,
  (SELECT COUNT(*) FROM rosters WHERE manager_email = :'email') AS roster_rows,
  (SELECT COUNT(*) FROM draft_queues WHERE manager_email = :'email') AS draft_queues,
  (SELECT COUNT(*) FROM draft_picks WHERE manager_email = :'email') AS draft_picks,
  (SELECT COUNT(*) FROM waiver_claims WHERE manager_email = :'email') AS waiver_claims,
  (SELECT COUNT(*) FROM waiver_priorities WHERE manager_email = :'email') AS waiver_priorities,
  (SELECT COUNT(*) FROM trade_offers WHERE offered_by_email = :'email' OR offered_to_email = :'email') AS trade_offers,
  (SELECT COUNT(*) FROM transactions WHERE manager_email = :'email') AS transactions,
  (SELECT COUNT(*) FROM league_feed_posts WHERE manager_email = :'email') AS feed_posts,
  (SELECT COUNT(*) FROM league_matchups WHERE home_manager_email = :'email' OR away_manager_email = :'email') AS matchups,
  (SELECT COUNT(*) FROM fantasy_player_scores WHERE manager_email = :'email') AS score_rows;
SQL
}

case "$command_name" in
  inspect)
    inspect_account
    ;;
  delete)
    [ "$confirm_email" = "$email" ] || {
      echo "Deletion requires --confirm-email with the exact canonical email" >&2
      exit 2
    }

    account_count="$(psql_base -tAc "SELECT COUNT(*) FROM users WHERE email = :'email'")"
    [ "$account_count" = "1" ] || {
      echo "No account exists for $email" >&2
      exit 3
    }

    related_count="$(psql_base -tAc "
      SELECT
        (SELECT COUNT(*) FROM leagues WHERE account_email = :'email') +
        (SELECT COUNT(*) FROM league_members WHERE email = :'email') +
        (SELECT COUNT(*) FROM rosters WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM draft_queues WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM draft_picks WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM waiver_claims WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM waiver_priorities WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM trade_offers WHERE offered_by_email = :'email' OR offered_to_email = :'email') +
        (SELECT COUNT(*) FROM transactions WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM league_feed_posts WHERE manager_email = :'email') +
        (SELECT COUNT(*) FROM league_matchups WHERE home_manager_email = :'email' OR away_manager_email = :'email') +
        (SELECT COUNT(*) FROM fantasy_player_scores WHERE manager_email = :'email')")"

    if [ "$related_count" != "0" ] && [ "$purge_related" != "true" ]; then
      echo "Refusing deletion: account has $related_count related league/fantasy rows." >&2
      echo "Run inspect first. Use --purge-related only for disposable test data." >&2
      exit 4
    fi

    if [ "$purge_related" = "true" ]; then
      psql_base <<'SQL'
BEGIN;

-- Owned leagues cascade through league-scoped tables.
DELETE FROM leagues WHERE account_email = :'email';

-- Remove this manager from leagues owned by someone else.
DELETE FROM trade_offers WHERE offered_by_email = :'email' OR offered_to_email = :'email';
DELETE FROM league_matchups WHERE home_manager_email = :'email' OR away_manager_email = :'email';
DELETE FROM fantasy_player_scores WHERE manager_email = :'email';
DELETE FROM league_feed_posts WHERE manager_email = :'email';
DELETE FROM transactions WHERE manager_email = :'email';
DELETE FROM waiver_claims WHERE manager_email = :'email';
DELETE FROM waiver_priorities WHERE manager_email = :'email';
DELETE FROM draft_picks WHERE manager_email = :'email';
DELETE FROM draft_queues WHERE manager_email = :'email';
DELETE FROM rosters WHERE manager_email = :'email';
UPDATE draft_states SET draft_order = array_remove(draft_order, :'email') WHERE :'email' = ANY(draft_order);
UPDATE leagues SET invited_emails = array_remove(invited_emails, :'email') WHERE :'email' = ANY(invited_emails);
DELETE FROM league_members WHERE email = :'email';
DELETE FROM auth_tokens WHERE email = :'email';
DELETE FROM users WHERE email = :'email';

COMMIT;
SQL
    else
      psql_base <<'SQL'
BEGIN;
DELETE FROM auth_tokens WHERE email = :'email';
DELETE FROM users WHERE email = :'email';
COMMIT;
SQL
    fi

    remaining="$(psql_base -tAc "SELECT COUNT(*) FROM users WHERE email = :'email'")"
    [ "$remaining" = "0" ] || { echo "Account deletion did not complete" >&2; exit 5; }
    echo "Deleted account: $email"
    ;;
  *)
    echo "Unknown command: $command_name" >&2
    usage >&2
    exit 2
    ;;
esac
