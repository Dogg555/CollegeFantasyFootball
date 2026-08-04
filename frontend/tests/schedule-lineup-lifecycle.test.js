const assert = require('assert');

class StorageMock {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.has(key) ? this.values.get(key) : null; }
  setItem(key, value) { this.values.set(key, String(value)); }
  removeItem(key) { this.values.delete(key); }
}

global.window = global;
global.localStorage = new StorageMock();
global.sessionStorage = new StorageMock();
global.CustomEvent = class CustomEvent { constructor(type, init) { this.type = type; this.detail = init?.detail; } };
global.document = {
  visibilityState: 'visible',
  querySelector: () => null,
  addEventListener: () => {}
};
global.addEventListener = () => {};
global.dispatchEvent = () => {};
global.setInterval = (callback) => { callback(); return 1; };
global.clearInterval = () => {};
global.setTimeout = () => 1;
global.BroadcastChannel = class BroadcastChannel {
  postMessage() {}
};

global.getLeagueState = () => ({ id: 'league-1' });
global.getAuthState = () => ({ token: 'token', email: 'manager@example.com' });
global.isLocalDemoSession = () => false;
global.saveMatchups = (matchups) => { global.savedMatchups = matchups; };
global.generateSeasonScheduleApi = async () => [];
global.updateRosterSlotApi = async () => true;
global.scoreWeekApi = async () => ({ ok: true });
global.finalizeWeekApi = async () => ({ ok: true });
global.apiRequest = async () => ({
  leagueId: 'league-1', season: 2026, week: 1, scheduleVersion: 1,
  schedule: [], myLineup: { status: 'open' }, lineupLocked: false
});

require('../schedule-lineup-lifecycle.js');

(async () => {
  const lifecycle = global.CFFScheduleLineupLifecycle;
  assert(lifecycle, 'lifecycle API should be exported');

  lifecycle.applyState({
    leagueId: 'league-1', season: 2026, week: 1, scheduleVersion: 5,
    schedule: [{ id: 'new' }], myLineup: { status: 'open' }, lineupLocked: false
  });
  lifecycle.applyState({
    leagueId: 'league-1', season: 2026, week: 1, scheduleVersion: 4,
    schedule: [{ id: 'stale' }], myLineup: { status: 'open' }, lineupLocked: false
  });
  assert.strictEqual(lifecycle.cachedState().scheduleVersion, 5, 'stale state must be ignored');
  assert.strictEqual(global.savedMatchups[0].id, 'new', 'stale schedule must not replace cache');

  let requestOptions = null;
  global.apiRequest = async (_path, options) => {
    requestOptions = options;
    return {
      leagueId: 'league-1', season: 2026, week: 1, scheduleVersion: 6,
      schedule: [{ id: 'generated' }], myLineup: { status: 'open' }, lineupLocked: false
    };
  };
  await lifecycle.mutate('generate', { season: 2026, week: 1, weeks: 12 }, { extra: '12' });
  assert(requestOptions.headers['Idempotency-Key'], 'mutation must send a stable operation key');
  assert.strictEqual(JSON.parse(requestOptions.body).expectedVersion, 5);
  assert.strictEqual(lifecycle.cachedState().scheduleVersion, 6);
  assert.strictEqual(global.sessionStorage.values.size, 0, 'confirmed mutation should clear operation key');

  lifecycle.applyState({
    leagueId: 'league-1', season: 2026, week: 1, scheduleVersion: 7,
    schedule: [], myLineup: { status: 'locked', lockReason: 'scoring' }, lineupLocked: true
  });
  await assert.rejects(
    () => global.updateRosterSlotApi('player-1', 'qb'),
    (error) => error?.status === 409 && error?.data?.code === 'lineup_locked'
  );

  global.apiRequest = async () => {
    const error = new Error('unavailable');
    error.status = 503;
    throw error;
  };
  await assert.rejects(
    () => lifecycle.mutate('lock', { season: 2026, week: 1 }, { extra: 'self' }),
    (error) => error?.status === 503
  );
  assert.strictEqual(global.sessionStorage.values.size, 1, 'uncertain mutation should retain operation key');

  console.log('schedule lineup browser contracts passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
