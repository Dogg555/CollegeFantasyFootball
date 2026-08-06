'use strict';

const assert = require('node:assert/strict');
const {
  createCoordinator,
  applyMutationResult,
  normalizePath,
  leagueIdFromPath
} = require('../reliable-sync.js');

function memoryStorage() {
  const values = new Map();
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); },
    dump(key) { return key ? values.get(key) : Object.fromEntries(values); }
  };
}

function rootFixture(storage, overrides = {}) {
  const scoped = {};
  const root = {
    localStorage: storage,
    navigator: { onLine: true },
    location: { href: 'https://example.test/league.html' },
    getAuthState: () => ({ email: 'manager@example.test', token: 'server-token' }),
    getLeagueState: () => ({ id: 'league-1', members: [] }),
    getLeaguesForCurrentAccount: () => [{ id: 'league-1' }],
    replaceLeaguesForCurrentAccount: () => {},
    setLeagueScopedItemsForLeague(key, leagueId, value) { scoped[`${key}:${leagueId}`] = value; },
    setRoster(value) { root.roster = value; },
    normalizePlayer: (player) => ({ ...player, normalized: true }),
    saveLeagueForAccount(value) { root.league = value; },
    applyDraftState(value) { root.draft = value; },
    dispatchEvent() {},
    addEventListener() {},
    document: { addEventListener() {}, visibilityState: 'visible' },
    setTimeout,
    ...overrides
  };
  root.scoped = scoped;
  return root;
}

async function testPartialRefreshAppliesSuccessfulCollections() {
  const storage = memoryStorage();
  const applied = [];
  const root = rootFixture(storage, {
    async apiRequest(path) {
      if (path === '/ok') return [{ id: 'player-1' }];
      const error = new Error('waivers unavailable');
      error.status = 503;
      error.unavailable = true;
      throw error;
    }
  });
  const coordinator = createCoordinator(root, {
    storage,
    resources: () => [
      { name: 'roster', path: '/ok', apply(value) { applied.push(['roster', value]); } },
      { name: 'waivers', path: '/bad', apply(value) { applied.push(['waivers', value]); } }
    ]
  });

  coordinator.initializeSafetyGate();
  assert.equal(coordinator.controlsDisabled(), true, 'writes remain gated before the first authoritative league read');
  const result = await coordinator.refreshActiveCollections({ force: true });
  assert.equal(result.partial, true);
  assert.deepEqual(result.applied, ['roster']);
  assert.deepEqual(result.failed, ['waivers']);
  assert.equal(applied.length, 1);
  assert.equal(coordinator.currentStatus().health, 'partial');
  assert.equal(coordinator.controlsDisabled(), true, 'partial authoritative state remains read-only until every required source refreshes');
}

async function testTotalFailureBlocksWrites() {
  const storage = memoryStorage();
  const root = rootFixture(storage, {
    async apiRequest() {
      const error = new Error('offline');
      error.unavailable = true;
      throw error;
    }
  });
  const coordinator = createCoordinator(root, {
    storage,
    resources: () => [{ name: 'roster', path: '/fail', apply() {} }]
  });

  await assert.rejects(
    () => coordinator.refreshActiveCollections({ force: true }),
    (error) => error.code === 'sync_refresh_failed'
  );
  assert.equal(coordinator.currentStatus().health, 'unavailable');
  assert.equal(coordinator.controlsDisabled(), true);
}

async function testCommittedMutationIsNotReportedAsFailed() {
  const storage = memoryStorage();
  const root = rootFixture(storage, {
    async apiRequest() {
      const error = new Error('refresh failed');
      error.status = 503;
      error.unavailable = true;
      throw error;
    }
  });
  const coordinator = createCoordinator(root, {
    storage,
    resources: () => [{ name: 'trades', path: '/fail', apply() {} }]
  });

  coordinator.recordCommittedMutation({
    path: '/leagues/league-1/trades',
    leagueId: 'league-1',
    requestId: 'request-1'
  });
  const result = await coordinator.refreshActiveCollections({ force: true });
  assert.equal(result.mutationCommitted, true);
  assert.equal(result.refreshFailed, true);
  assert.equal(result.error.mutationCommitted, true);
  assert.equal(result.error.requestId, 'request-1');
  assert.equal(coordinator.controlsDisabled(), true, 'further writes are gated until the server can be read again');
}

async function testImmediateDuplicateRefreshIsCoalesced() {
  const storage = memoryStorage();
  let calls = 0;
  const root = rootFixture(storage, {
    async apiRequest() {
      calls += 1;
      return [{ id: 'one' }];
    }
  });
  const coordinator = createCoordinator(root, {
    storage,
    resources: () => [{ name: 'roster', path: '/roster', apply() {} }]
  });

  const first = await coordinator.refreshActiveCollections({ force: true });
  const second = await coordinator.refreshActiveCollections();
  assert.equal(first.ok, true);
  assert.equal(second.ok, true);
  assert.equal(calls, 1, 'legacy post-mutation refresh does not issue the same reads twice');
}

async function testOlderRefreshCannotOverwriteNewerState() {
  const storage = memoryStorage();
  let call = 0;
  let releaseFirst;
  const firstResponse = new Promise((resolve) => { releaseFirst = resolve; });
  const applied = [];
  const root = rootFixture(storage, {
    async apiRequest() {
      call += 1;
      if (call === 1) return firstResponse;
      return ['new'];
    }
  });
  const coordinator = createCoordinator(root, {
    storage,
    resources: () => [{ name: 'roster', path: '/roster', apply(value) { applied.push(value[0]); } }]
  });

  const older = coordinator.refreshActiveCollections({ force: true });
  await Promise.resolve();
  const newer = await coordinator.refreshActiveCollections({ force: true });
  releaseFirst(['old']);
  const superseded = await older;

  assert.equal(newer.ok, true);
  assert.equal(superseded.superseded, true);
  assert.deepEqual(applied, ['new']);
}

function testMutationResponseHydratesAuthoritativeCaches() {
  const storage = memoryStorage();
  const root = rootFixture(storage);
  const applied = applyMutationResult(root, '/leagues/league-1/trades/trade-1/status', {
    offers: [{ id: 'trade-1', status: 'Approved' }],
    roster: [{ id: 'player-2' }]
  });
  assert.deepEqual(applied.sort(), ['roster', 'trades']);
  assert.equal(root.roster[0].normalized, true);
  assert.equal(root.scoped['cff_trades_by_league:league-1'][0].status, 'Approved');
}

async function testSuccessfulMutationIsRecordedByApiWrapper() {
  const storage = memoryStorage();
  const root = rootFixture(storage, {
    async apiRequest() {
      return { offers: [{ id: 'trade-1' }], roster: [{ id: 'player-3' }] };
    }
  });
  const coordinator = createCoordinator(root, { storage });
  assert.equal(coordinator.installApiWrapper(), true);
  await root.apiRequest('/leagues/league-1/trades/trade-1/status', { method: 'POST', cffRequestId: 'req-2' });
  assert.equal(coordinator.hasRecentCommit('league-1'), true);
  assert.equal(coordinator.currentStatus().lastMutationRequestId, 'req-2');
  assert.equal(root.roster[0].id, 'player-3');
}

async function main() {
  assert.equal(normalizePath('https://example.test/api/leagues/a/trades'), '/leagues/a/trades');
  assert.equal(leagueIdFromPath('/leagues/a%20b/roster'), 'a b');
  await testPartialRefreshAppliesSuccessfulCollections();
  await testTotalFailureBlocksWrites();
  await testCommittedMutationIsNotReportedAsFailed();
  await testImmediateDuplicateRefreshIsCoalesced();
  await testOlderRefreshCannotOverwriteNewerState();
  testMutationResponseHydratesAuthoritativeCaches();
  await testSuccessfulMutationIsRecordedByApiWrapper();
  console.log('Reliable server synchronization contracts passed.');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
