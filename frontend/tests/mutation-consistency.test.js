'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');

const {
  DATA_REVISION_KEY,
  requestPolicy,
  leagueIdFromPath,
  markScopesStale,
  purgeLeagueCaches,
  resultLeagueId,
  createCoordinator
} = require(path.join('..', 'mutation-consistency.js'));

function memoryStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); },
    snapshot() { return Object.fromEntries(values.entries()); }
  };
}

assert.deepEqual(requestPolicy('/leagues', 'POST').scopes, ['leagues', 'league', 'draft']);
assert.equal(requestPolicy('/leagues/l-1/draft/queue', 'PUT').key, 'draft-queue');
assert.deepEqual(requestPolicy('/leagues/l-1/roster/drop', 'POST').scopes, ['league']);
assert.deepEqual(requestPolicy('/leagues/l-1/members/user%40example.test', 'PUT').scopes, ['leagues', 'league', 'draft']);
assert.equal(requestPolicy('/leagues/l-1', 'DELETE').purgeLeagueId, 'l-1');
assert.equal(requestPolicy('/auth/login', 'POST'), null);
assert.equal(requestPolicy('/leagues', 'GET'), null);
assert.equal(leagueIdFromPath('/api/leagues/league%201/draft'), 'league 1');
assert.equal(resultLeagueId({ joinStatus: 'pending_approval', id: 'ignored' }), '');
assert.equal(resultLeagueId({ id: 'league-2' }), 'league-2');

const staleStorage = memoryStorage();
markScopesStale(staleStorage, ['league', 'draft'], {
  mutationId: 'mutation-1',
  leagueId: 'league-1',
  now: '2026-08-04T15:00:00.000Z'
});
const staleMeta = JSON.parse(staleStorage.getItem('cff_api_cache_meta'));
assert.equal(staleMeta.league.stale, true);
assert.equal(staleMeta.draft.invalidatedBy, 'mutation-1');

const purgeStorage = memoryStorage({
  cff_matchups_by_league: JSON.stringify({ 'league-1': [{ id: 1 }], 'league-2': [{ id: 2 }] }),
  cff_draft_meta_by_league: JSON.stringify({ 'league-1': { status: 'open' } })
});
purgeLeagueCaches(purgeStorage, 'league-1');
assert.deepEqual(JSON.parse(purgeStorage.getItem('cff_matchups_by_league')), { 'league-2': [{ id: 2 }] });
assert.deepEqual(JSON.parse(purgeStorage.getItem('cff_draft_meta_by_league')), {});

(async () => {
  const storage = memoryStorage();
  const calls = [];
  let activeLeagueId = 'league-1';
  const root = {
    localStorage: storage,
    getAuthState: () => ({ email: 'manager@example.test', token: 'server-token' }),
    getLeagueState: () => activeLeagueId ? { id: activeLeagueId } : null,
    setActiveLeague: (leagueId) => { calls.push(`activate:${leagueId}`); activeLeagueId = leagueId; },
    syncLeaguesFromApi: async () => { calls.push('refresh:leagues'); },
    syncActiveLeagueCollectionsFromApi: async () => { calls.push(`refresh:league:${activeLeagueId}`); },
    syncDraftFromApi: async () => { calls.push(`refresh:draft:${activeLeagueId}`); return {}; },
    writeApiCacheMeta: (scope, leagueId) => { calls.push(`fresh:${scope}:${leagueId}`); },
    apiRequest: async (requestPath, options = {}) => {
      calls.push(`request:${options.method || 'GET'}:${requestPath}`);
      if (requestPath === '/leagues' && options.method === 'POST') return { id: 'league-2' };
      return { ok: true };
    },
    setTimeout: (callback) => { callback(); return 1; },
    addEventListener: () => {},
    dispatchEvent: () => {},
    renderLeague: () => { calls.push('render:league'); }
  };

  const coordinator = createCoordinator(root, {
    storage,
    createId: () => 'mutation-create',
    setTimeout: (callback) => { callback(); return 1; }
  });
  assert.equal(coordinator.install(), true);

  const created = await root.apiRequest('/leagues', { method: 'POST' });
  assert.equal(created.id, 'league-2');
  assert.ok(calls.indexOf('request:POST:/leagues') < calls.indexOf('refresh:leagues'));
  assert.ok(calls.indexOf('refresh:leagues') < calls.indexOf('activate:league-2'));
  assert.ok(calls.indexOf('activate:league-2') < calls.indexOf('refresh:league:league-2'));
  assert.ok(calls.indexOf('refresh:league:league-2') < calls.indexOf('refresh:draft:league-2'));
  assert.ok(calls.includes('fresh:draft:league-2'));
  assert.equal(JSON.parse(storage.getItem(DATA_REVISION_KEY)).status, 'refreshed');

  const beforeGetRefreshes = calls.filter((item) => item.startsWith('refresh:')).length;
  await root.apiRequest('/leagues/league-2/roster');
  assert.equal(calls.filter((item) => item.startsWith('refresh:')).length, beforeGetRefreshes, 'reads must not trigger mutation refresh');

  const beforeRosterLeagueRefreshes = calls.filter((item) => item.startsWith('refresh:league')).length;
  await root.apiRequest('/leagues/league-2/roster/drop', { method: 'POST' });
  assert.equal(calls.filter((item) => item.startsWith('refresh:league')).length, beforeRosterLeagueRefreshes + 1);

  const demoCalls = [];
  const demoRoot = {
    localStorage: memoryStorage(),
    getAuthState: () => ({ email: 'demo@example.test', token: 'local-demo-test' }),
    getLeagueState: () => ({ id: 'local-1' }),
    syncActiveLeagueCollectionsFromApi: async () => demoCalls.push('refresh'),
    apiRequest: async () => ({ ok: true }),
    setTimeout: (callback) => { callback(); return 1; },
    addEventListener: () => {}
  };
  const demoCoordinator = createCoordinator(demoRoot, { storage: demoRoot.localStorage });
  demoCoordinator.install();
  await demoRoot.apiRequest('/leagues/local-1/roster', { method: 'POST' });
  assert.deepEqual(demoCalls, [], 'localhost demo sessions must retain local behavior without server refresh');

  const failureStorage = memoryStorage();
  const warnings = [];
  const failureRoot = {
    localStorage: failureStorage,
    getAuthState: () => ({ email: 'manager@example.test', token: 'server-token' }),
    getLeagueState: () => ({ id: 'league-failure' }),
    syncActiveLeagueCollectionsFromApi: async () => { throw new Error('refresh unavailable'); },
    apiRequest: async () => ({ committed: true }),
    CFFAsyncStates: { show: (...args) => warnings.push(args) },
    setTimeout: (callback) => { callback(); return 1; },
    addEventListener: () => {},
    dispatchEvent: () => {}
  };
  const failureCoordinator = createCoordinator(failureRoot, {
    storage: failureStorage,
    createId: () => 'mutation-failure',
    setTimeout: (callback) => { callback(); return 1; }
  });
  failureCoordinator.install();
  const committed = await failureRoot.apiRequest('/leagues/league-failure/score/week/1', { method: 'POST' });
  assert.equal(committed.committed, true, 'a committed mutation must not be reported as unsaved when only refresh fails');
  assert.equal(JSON.parse(failureStorage.getItem('cff_api_cache_meta')).league.stale, true);
  assert.equal(JSON.parse(failureStorage.getItem(DATA_REVISION_KEY)).status, 'refresh-failed');
  assert.equal(warnings[0][0], 'warning');
  assert.match(warnings[0][1], /refresh incomplete/i);

  console.log('mutation consistency runtime tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
