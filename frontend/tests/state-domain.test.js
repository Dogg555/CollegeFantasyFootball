'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const stateSource = fs.readFileSync(path.join(__dirname, '..', 'state.js'), 'utf8');
const context = {
  console,
  Date,
  URLSearchParams,
  fetch: async () => ({ ok: true, json: async () => ({}) }),
  localStorage: {
    values: new Map(),
    getItem(key) { return this.values.has(key) ? this.values.get(key) : null; },
    setItem(key, value) { this.values.set(key, String(value)); },
    removeItem(key) { this.values.delete(key); }
  },
  sessionStorage: {
    values: new Map(),
    getItem(key) { return this.values.has(key) ? this.values.get(key) : null; },
    setItem(key, value) { this.values.set(key, String(value)); },
    removeItem(key) { this.values.delete(key); }
  },
  window: {
    CFF_API_BASE: '/api',
    CFF_ALLOW_LOCAL_DEMO: false,
    CFF_AUTH_VALIDATE_TIMEOUT_MS: 100,
    clearTimeout,
    location: { hostname: 'localhost' },
    setTimeout
  }
};
context.window.localStorage = context.localStorage;
context.window.sessionStorage = context.sessionStorage;
context.globalThis = context;
vm.createContext(context);
vm.runInContext(
  stateSource,
  context
);

const league = {
  members: [
    { email: 'active@example.test', status: 'Active', teamName: 'Active' },
    { email: 'upper@example.test', status: 'ACTIVE', teamName: 'Upper' },
    { email: 'invited@example.test', status: 'Invited', teamName: 'Invited' },
    { email: 'pending@example.test', status: 'Pending', teamName: 'Pending' },
    { email: 'removed@example.test', status: 'Removed', teamName: 'Removed' }
  ]
};

const standings = context.standingsFromMatchups(league, [
  {
    homeManager: 'active@example.test',
    awayManager: 'upper@example.test',
    homeScore: 35,
    awayScore: 28,
    status: 'final'
  },
  {
    homeManager: 'invited@example.test',
    awayManager: 'pending@example.test',
    homeScore: 99,
    awayScore: 1,
    status: 'final'
  }
]);

assert.deepEqual(
  context.activeLeagueManagers(league).map((member) => member.email).sort(),
  ['active@example.test', 'upper@example.test'],
  'local schedules must include active managers only'
);
assert.deepEqual(
  standings.map((row) => row.email).sort(),
  ['active@example.test', 'upper@example.test'],
  'local standings must include active managers only'
);
assert.equal(standings.find((row) => row.email === 'active@example.test').wins, 1);
assert.equal(context.sameSeasonWeek({ season: 2026, week: 1 }, 2026, 1), true);
assert.equal(context.sameSeasonWeek({ season: 2025, week: 1 }, 2026, 1), false);

assert.match(
  stateSource,
  /if \(!getAuthState\(\)\?\.token \|\| isLocalDemoSession\(\)\) \{[\s\S]*?samplePlayers\.filter/,
  'manager roster demo fallback must be limited to missing-auth or explicit local demo mode'
);
assert.match(
  stateSource,
  /if \(!league\?\.id \|\| !managerEmail\) return \[\];/,
  'production manager roster lookup must not synthesize sample rosters when scoped data is missing'
);

context.setAuthState({ email: 'owner@example.test', token: 'server-token' });
context.saveLeagueForAccount({ id: 'league-a', name: 'League A' }, { activate: true });
context.saveLeagueForAccount({ id: 'league-b', name: 'League B' });
assert.equal(
  context.getLeagueState().id,
  'league-a',
  'background league saves must not change the selected league'
);
context.saveLeagueForAccount({ id: 'league-b', name: 'League B Updated' }, { activate: true });
assert.equal(
  context.getLeagueState().id,
  'league-b',
  'explicit activation must switch the selected league'
);
assert.equal(context.getLeaguesForCurrentAccount().length, 2, 'multiple active leagues must remain stored');

console.log('state domain tests passed');
