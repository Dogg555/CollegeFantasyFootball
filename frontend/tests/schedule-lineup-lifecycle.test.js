'use strict';

const assert = require('assert');
const lifecycle = require('../schedule-lineup-lifecycle.js');

class MemoryStorage {
  constructor() { this.values = new Map(); }
  getItem(key) { return this.values.has(key) ? this.values.get(key) : null; }
  setItem(key, value) { this.values.set(key, String(value)); }
  removeItem(key) { this.values.delete(key); }
}

const storage = new MemoryStorage();
let ids = 0;
const createId = () => `operation-${++ids}`;
const first = lifecycle.operationFor('generate', 'league-1', 2026, 1, 'same', storage, createId);
const replay = lifecycle.operationFor('generate', 'league-1', 2026, 1, 'same', storage, createId);
assert.strictEqual(first.operationKey, replay.operationKey, 'uncertain retries must reuse the operation key');
const changed = lifecycle.operationFor('generate', 'league-1', 2026, 1, 'changed', storage, createId);
assert.notStrictEqual(first.operationKey, changed.operationKey, 'changed inputs need a new operation key');

lifecycle.clearOperation('generate', 'league-1', 2026, 1, changed.operationKey, storage);
assert.strictEqual(JSON.parse(storage.getItem(lifecycle.OPERATION_STORAGE_KEY) || '{}')['league-1:generate:2026:1'], undefined);

assert.strictEqual(lifecycle.shouldApplyState({ version: 4 }, { version: 3 }), false);
assert.strictEqual(lifecycle.shouldApplyState({ version: 4 }, { scheduleVersion: 5 }), true);
assert.strictEqual(lifecycle.uncertainFailure({ status: 503 }), true);
assert.strictEqual(lifecycle.uncertainFailure({ status: 409 }), false);

const state = {
  currentWeek: 2,
  weekControls: [
    { week: 1, status: 'finalized' },
    { week: 2, status: 'locked' },
    { week: 3, status: 'open' }
  ]
};
assert.strictEqual(lifecycle.controlForWeek(2, state).status, 'locked');
assert.strictEqual(lifecycle.lockedForWeek(1, state), true);
assert.strictEqual(lifecycle.lockedForWeek(2, state), true);
assert.strictEqual(lifecycle.lockedForWeek(3, state), false);
assert.match(lifecycle.scheduleErrorMessage({ code: 'schedule_state_conflict' }), /latest version/i);
assert.match(lifecycle.scheduleErrorMessage({ code: 'lineup_locked' }), /locked/i);

console.log('schedule lineup browser lifecycle passed');
