'use strict';

const assert = require('node:assert/strict');
const {
  normalizeVersion,
  shouldApplyState,
  createOperationId,
  operationFor,
  clearOperation,
  uncertainFailure,
  waiverErrorMessage
} = require('../waiver-lifecycle.js');

function memoryStorage() {
  const values = new Map();
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); }
  };
}

function testVersionContracts() {
  assert.equal(normalizeVersion('4'), 4);
  assert.equal(normalizeVersion(-1), 0);
  assert.equal(shouldApplyState({ version: 4 }, { version: 5 }), true);
  assert.equal(shouldApplyState({ version: 5 }, { version: 4 }), false);
  assert.equal(shouldApplyState(null, { version: 0 }), true);
}

function testOperationReuse() {
  const storage = memoryStorage();
  let next = 0;
  const createId = () => `operation-${++next}`;
  const first = operationFor('create', 'league-1', 'player-a', storage, createId);
  const replay = operationFor('create', 'league-1', 'player-a', storage, createId);
  assert.equal(replay.operationKey, first.operationKey, 'uncertain retry must reuse operation key');
  const different = operationFor('create', 'league-1', 'player-b', storage, createId);
  assert.notEqual(different.operationKey, first.operationKey, 'different claim must receive a new operation key');
  clearOperation('create', 'league-1', different.operationKey, storage);
  const afterClear = operationFor('create', 'league-1', 'player-b', storage, createId);
  assert.notEqual(afterClear.operationKey, different.operationKey, 'confirmed action must clear stored operation');
}

function testFallbackOperationId() {
  const id = createOperationId(null, () => 1234, () => 0.5);
  assert.match(id, /^waiver-/);
}

function testUncertainFailures() {
  assert.equal(uncertainFailure({ timedOut: true }), true);
  assert.equal(uncertainFailure({ status: 503 }), true);
  assert.equal(uncertainFailure({ status: 409 }), false);
  assert.equal(uncertainFailure({ status: 400 }), false);
}

function testMessages() {
  assert.match(waiverErrorMessage({ data: { code: 'waiver_state_conflict' } }), /latest server state/i);
  assert.match(waiverErrorMessage({ data: { code: 'player_unavailable' } }), /another manager/i);
  assert.match(waiverErrorMessage({ timedOut: true }), /same operation/i);
  assert.match(waiverErrorMessage({ data: { code: 'waiver_claim_out_of_order' } }), /priority order/i);
}

function main() {
  testVersionContracts();
  testOperationReuse();
  testFallbackOperationId();
  testUncertainFailures();
  testMessages();
  console.log('waiver lifecycle browser contracts passed');
}

main();
