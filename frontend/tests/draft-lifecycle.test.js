'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');

const {
  normalizeVersion,
  snapshotVersion,
  shouldApplySnapshot,
  createOperationId,
  operationFor,
  clearOperation,
  uncertainFailure,
  draftErrorMessage
} = require(path.join('..', 'draft-lifecycle.js'));

class MemoryStorage {
  constructor() {
    this.values = new Map();
  }
  getItem(key) {
    return this.values.has(key) ? this.values.get(key) : null;
  }
  setItem(key, value) {
    this.values.set(key, String(value));
  }
}

assert.equal(normalizeVersion('12'), 12);
assert.equal(normalizeVersion(-1), 0);
assert.equal(snapshotVersion({ revision: 7 }), 7);
assert.equal(shouldApplySnapshot({ version: 8 }, { version: 7 }), false);
assert.equal(shouldApplySnapshot({ version: 8 }, { version: 8 }), true);
assert.equal(shouldApplySnapshot({ version: 8 }, { version: 9 }), true);
assert.match(createOperationId(null, () => 1000, () => 0.25), /^draft-/);

const storage = new MemoryStorage();
let sequence = 0;
const createId = () => `operation-${++sequence}`;
const first = operationFor('pick', 'league-1', 'player-1', storage, createId);
const replay = operationFor('pick', 'league-1', 'player-1', storage, createId);
assert.equal(first.operationKey, 'operation-1');
assert.equal(replay.operationKey, first.operationKey, 'uncertain retry must reuse one operation key');

const changed = operationFor('pick', 'league-1', 'player-2', storage, createId);
assert.equal(changed.operationKey, 'operation-2');
clearOperation('pick', 'league-1', 'wrong-key', storage);
assert.equal(operationFor('pick', 'league-1', 'player-2', storage, createId).operationKey, 'operation-2');
clearOperation('pick', 'league-1', 'operation-2', storage);
assert.equal(operationFor('pick', 'league-1', 'player-2', storage, createId).operationKey, 'operation-3');

assert.equal(uncertainFailure({ timedOut: true }), true);
assert.equal(uncertainFailure({ status: 503 }), true);
assert.equal(uncertainFailure({ status: 409 }), false);
assert.match(draftErrorMessage({ data: { code: 'draft_state_conflict' } }), /latest board/i);
assert.match(draftErrorMessage({ data: { code: 'player_already_drafted' } }), /selected by another manager/i);
assert.match(draftErrorMessage({ timedOut: true }), /Retry safely/i);

console.log('draft lifecycle runtime tests passed');
