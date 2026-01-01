# Recommended `stat_name` keys by category

These keys are intended to keep ingestion and scoring consistent across passing, rushing, receiving, and a reserved defense category. Use them as-is (lowerCamelCase) in `player_stats.stat_name`.

## Passing (`category='passing'`)
- `passAttempts`
- `passCompletions`
- `passYards`
- `passTD`
- `passInt`
- `sackYards`
- `sackTaken`
- `passTwoPt`

## Rushing (`category='rushing'`)
- `rushAttempts`
- `rushYards`
- `rushTD`
- `rushFumbles`
- `rushTwoPt`

## Receiving (`category='receiving'`)
- `targets`
- `receptions`
- `recYards`
- `recTD`
- `recFumbles`
- `recTwoPt`

## Defense (`category='defense'`, reserved for future ingestion)
- `defSacks`
- `defInterceptions`
- `defFumbleRecoveries`
- `defTD`
- `defSafeties`
- `defPointsAllowed`

### Notes
- Keep `category` set explicitly to one of the four values above. Defense is defined but should not be populated until defensive data ingestion is enabled.
- `stat_value` is stored as `NUMERIC` to handle fractional points or averaged stats when needed.
