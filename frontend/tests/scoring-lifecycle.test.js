'use strict';

const assert = require('node:assert/strict');
const scoring = require('../scoring-lifecycle.js');

function memoryStorage() {
  const values = new Map();
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); }
  };
}

assert.equal(scoring.normalizeVersion('4'), 4);
assert.equal(scoring.normalizeVersion(-1), 0);
assert.equal(scoring.stateVersion({ weekVersion: 3, version: 2 }), 3);
assert.equal(scoring.globalVersion({ globalVersion: 8 }), 8);
assert.equal(scoring.standingsVersion({ standingsVersion: 5 }), 5);

assert.equal(scoring.shouldApplyState(null, { globalVersion: 1, weekVersion: 1 }), true);
assert.equal(scoring.shouldApplyState(
  { globalVersion: 3, weekVersion: 2, standingsVersion: 1 },
  { globalVersion: 2, weekVersion: 99, standingsVersion: 99 }
), false);
assert.equal(scoring.shouldApplyState(
  { globalVersion: 3, weekVersion: 2, standingsVersion: 1 },
  { globalVersion: 3, weekVersion: 3, standingsVersion: 1 }
), true);
assert.equal(scoring.shouldApplyState(
  { globalVersion: 3, weekVersion: 3, standingsVersion: 4 },
  { globalVersion: 3, weekVersion: 3, standingsVersion: 2 }
), false);

const storage = memoryStorage();
let operationIds = 0;
const first = scoring.operationFor('score', 'league-1', 2026, 1, 'score:0', storage, () => `op-${++operationIds}`);
const retry = scoring.operationFor('score', 'league-1', 2026, 1, 'score:0', storage, () => `op-${++operationIds}`);
assert.equal(first.operationKey, retry.operationKey, 'uncertain retry must reuse the same operation key');
const changed = scoring.operationFor('score', 'league-1', 2026, 1, 'score:1', storage, () => `op-${++operationIds}`);
assert.notEqual(changed.operationKey, first.operationKey, 'changed confirmed state needs a new operation key');
const finalize = scoring.operationFor('finalize', 'league-1', 2026, 1, 'finalize:1', storage, () => `op-${++operationIds}`);
assert.notEqual(finalize.operationKey, changed.operationKey, 'score and finalize operations must remain distinct');
scoring.clearOperation('finalize', 'league-1', 2026, 1, finalize.operationKey, storage);
const finalizeAfterClear = scoring.operationFor('finalize', 'league-1', 2026, 1, 'finalize:1', storage, () => `op-${++operationIds}`);
assert.notEqual(finalizeAfterClear.operationKey, finalize.operationKey);

assert.equal(scoring.uncertainFailure({ status: 503 }), true);
assert.equal(scoring.uncertainFailure({ timedOut: true }), true);
assert.equal(scoring.uncertainFailure({ status: 409 }), false);
assert.match(scoring.scoringErrorMessage({ data: { code: 'week_finalized' } }), /final/i);
assert.match(scoring.scoringErrorMessage({ data: { code: 'scoring_state_conflict' } }), /latest/i);
assert.match(scoring.scoringErrorMessage({ data: { code: 'invalid_lineup' } }), /lineup/i);
assert.match(scoring.scoringErrorMessage({ status: 503 }), /same operation/i);

console.log('scoring lifecycle browser contracts passed');
