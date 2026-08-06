#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
config = (ROOT / 'frontend' / 'config.js').read_text(encoding='utf-8')
runtime = (ROOT / 'frontend' / 'runtime-compat.js').read_text(encoding='utf-8')
sync = (ROOT / 'frontend' / 'reliable-sync.js').read_text(encoding='utf-8')

for source in (config, runtime):
    assert 'mutationControlsDisabled = () => false' not in source, 'authenticated mutation fallback must fail closed'
    assert 'mutationCommitted' in source, 'saved mutation / failed refresh message must be distinct'

for marker in (
    "'mutation-consistency.js', 'reliable-sync.js'",
    'syncActiveLeagueCollectionsFromApi = refreshActiveCollections',
    'syncLeaguesFromApi = refreshLeagues',
    'syncDraftFromApi = refreshDraft',
    'Promise.allSettled',
    'mutationCommitted',
    'REFRESH_DEDUPE_MS',
    'superseded: true',
    "addEventListener?.('online'",
    "addEventListener?.('offline'",
    "event.key !== DATA_REVISION_KEY",
):
    assert marker in config or marker in sync, f'missing reliable synchronization contract: {marker}'

assert config.count("'reliable-sync.js'") == 2, 'reliable sync must load in both dependency lists'
assert config.index("'mutation-consistency.js', 'reliable-sync.js'") < config.index("'snake-draft-only.js'"), (
    'reliable sync must load after mutation consistency and before feature lifecycle modules'
)
assert "health: 'unavailable'" in sync and 'writable: false' in sync
assert "health: complete ? 'healthy' : 'partial'" in sync
assert 'older' not in sync.lower() or 'superseded' in sync

print('Reliable synchronization source contracts passed.')
