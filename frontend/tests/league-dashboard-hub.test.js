'use strict';

const assert = require('node:assert/strict');
const helpers = require('../league-dashboard-hub.js');

function sample(overrides = {}) {
  return {
    leagueId: 'league-1',
    league: { id: 'league-1', name: 'Saturday League' },
    nextAction: { code: 'fix_lineup', label: 'Fix your lineup', href: 'league.html#team', detail: 'One starter is missing.' },
    lineup: { status: 'incomplete', rosterCount: 6, warnings: [{ missing: 1, message: 'Missing 1 rb starter.' }] },
    currentMatchup: { week: 2, opponentTeamName: 'Cowboys', scoreFor: 12.5, scoreAgainst: 8, status: 'scheduled' },
    waivers: { pendingCount: 2, items: [] },
    trades: { actionRequiredCount: 1, items: [] },
    standings: { myTeam: { rank: 3, teamName: 'Pokes', wins: 1, losses: 1, ties: 0, pointsFor: 42.25 }, leaders: [] },
    activity: [],
    commissionerNotices: [],
    deadlines: [{ type: 'lineup', label: 'Lineup deadline', at: '2026-09-05T16:00:00Z' }],
    freshness: { source: 'api', generatedAt: '2026-09-05T15:59:30Z', stale: false, partial: false },
    ...overrides
  };
}

function memoryStorage() {
  const values = new Map();
  return {
    getItem: (key) => values.get(key) || null,
    setItem: (key, value) => values.set(key, value),
    removeItem: (key) => values.delete(key)
  };
}

function testViewModel() {
  const view = helpers.dashboardViewModel(sample(), Date.parse('2026-09-05T16:00:00Z'));
  assert.equal(view.nextAction.label, 'Fix your lineup');
  assert.equal(view.matchup.title, 'Week 2 vs Cowboys');
  assert.equal(view.matchup.detail, '12.5–8 · scheduled');
  assert.equal(view.lineup.title, '1 starter slot empty');
  assert.equal(view.pending.title, '3 items need attention');
  assert.equal(view.standings.title, '#3 Pokes');
  assert.equal(view.freshness, 'Updated now');
}

function testPartialAndStaleLabels() {
  assert.match(helpers.freshnessLabel(helpers.normalizeDashboard(sample({
    freshness: { generatedAt: '2026-09-05T15:50:00Z', partial: true }
  })), Date.parse('2026-09-05T16:00:00Z')), /partial/);
  assert.equal(helpers.freshnessLabel(helpers.normalizeDashboard(sample({ readOnly: true }))), 'Read-only cached dashboard');
}

function testCacheIsAccountAndLeagueScoped() {
  const storage = memoryStorage();
  helpers.saveCache(storage, 'Owner@Example.com', 'league-1', sample(), 1000);
  const cached = helpers.loadCache(storage, 'owner@example.com', 'league-1', 2000);
  assert.equal(cached.leagueId, 'league-1');
  assert.equal(cached.readOnly, true);
  assert.equal(cached.freshness.stale, true);
  assert.equal(helpers.loadCache(storage, 'other@example.com', 'league-1', 2000), null);
  assert.equal(helpers.loadCache(storage, 'owner@example.com', 'league-2', 2000), null);
}

function testCacheCanBeClearedByExactScope() {
  const storage = memoryStorage();
  helpers.saveCache(storage, 'owner@example.com', 'league-1', sample(), 1000);
  helpers.saveCache(storage, 'owner@example.com', 'league-2', sample({ leagueId: 'league-2' }), 1000);
  helpers.clearCache(storage, 'owner@example.com', 'league-1');
  assert.equal(helpers.loadCache(storage, 'owner@example.com', 'league-1', 2000), null);
  assert.equal(helpers.loadCache(storage, 'owner@example.com', 'league-2', 2000).leagueId, 'league-2');
}

function testAuthorizationFailuresNeverQualifyForFallback() {
  for (const status of [401, 403, 404]) {
    assert.equal(helpers.isAuthorizationFailure({ status }), true);
  }
  for (const status of [0, 408, 429, 500, 503]) {
    assert.equal(helpers.isAuthorizationFailure({ status }), false);
  }
}

function testEmptyStates() {
  const view = helpers.dashboardViewModel(sample({
    currentMatchup: null,
    lineup: { status: 'pre_draft', warnings: [], rosterCount: 0 },
    waivers: {},
    trades: {},
    standings: {},
    deadlines: []
  }));
  assert.equal(view.matchup.title, 'No matchup scheduled');
  assert.equal(view.lineup.title, 'Lineup opens after the draft');
  assert.equal(view.pending.title, 'No pending actions');
  assert.equal(view.standings.title, 'Standings unavailable');
}

testViewModel();
testPartialAndStaleLabels();
testCacheIsAccountAndLeagueScoped();
testCacheCanBeClearedByExactScope();
testAuthorizationFailuresNeverQualifyForFallback();
testEmptyStates();
console.log('league dashboard hub frontend tests passed');
