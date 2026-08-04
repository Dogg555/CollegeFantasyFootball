'use strict';

const assert = require('node:assert/strict');

const memoryStorage = () => {
  const values = new Map();
  return {
    getItem: (key) => values.has(key) ? values.get(key) : null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key)
  };
};

globalThis.sessionStorage = memoryStorage();
globalThis.localStorage = memoryStorage();
globalThis.setTimeout = (callback) => {
  callback();
  return 1;
};

const helpers = require('../roster-transactions.js');

function testVersionOrdering() {
  assert.equal(helpers.normalizeVersion('7'), 7);
  assert.equal(helpers.normalizeVersion(-1), 0);
  assert.equal(helpers.shouldApplyState(null, { version: 1, roster: [] }), true);
  assert.equal(helpers.shouldApplyState({ version: 3, roster: [] }, { version: 2, roster: [] }), false);
  assert.equal(helpers.shouldApplyState({ version: 3, roster: [] }, { version: 3, roster: [] }), true);
  assert.equal(helpers.shouldApplyState({ version: 1, roster: [] }, { version: 2 }), false);
}

function testOperationReuse() {
  const storage = memoryStorage();
  let generated = 0;
  const createId = () => `operation-${++generated}`;
  const first = helpers.operationFor('swap', 'league-1', 'add:drop', storage, createId);
  const second = helpers.operationFor('swap', 'league-1', 'add:drop', storage, createId);
  assert.equal(first.operationKey, second.operationKey);
  const changed = helpers.operationFor('swap', 'league-1', 'other:drop', storage, createId);
  assert.notEqual(changed.operationKey, first.operationKey);
  helpers.clearOperation('swap', 'league-1', changed.operationKey, storage);
  const afterClear = helpers.operationFor('swap', 'league-1', 'other:drop', storage, createId);
  assert.notEqual(afterClear.operationKey, changed.operationKey);
}

async function testUncertainRetryUsesSameRequest() {
  const calls = [];
  globalThis.apiRequest = async (path, options) => {
    calls.push({ path, options });
    if (calls.length === 1) {
      const error = new Error('connection reset');
      error.unavailable = true;
      throw error;
    }
    return { version: 4, roster: [] };
  };
  const options = {
    method: 'POST',
    headers: { 'Idempotency-Key': 'same-key' },
    body: '{"action":"add"}'
  };
  const result = await helpers.requestWithUncertainRetry('/leagues/one/roster/transactions', options);
  assert.equal(result.version, 4);
  assert.equal(calls.length, 2);
  assert.equal(calls[0].options, options);
  assert.equal(calls[1].options, options);
  assert.equal(calls[1].options.headers['Idempotency-Key'], 'same-key');
}

function testFailureClassificationAndMessages() {
  assert.equal(helpers.uncertainFailure({ status: 0 }), true);
  assert.equal(helpers.uncertainFailure({ status: 503 }), true);
  assert.equal(helpers.uncertainFailure({ status: 409 }), false);
  assert.match(
    helpers.rosterErrorMessage({ data: { code: 'waiver_claim_required' } }),
    /waiver claim/i
  );
  assert.match(
    helpers.rosterErrorMessage({ data: { code: 'roster_state_conflict' } }),
    /latest roster/i
  );
  assert.match(
    helpers.rosterErrorMessage({ data: { code: 'player_unavailable' } }),
    /another manager/i
  );
}

function testStoredVersions() {
  const storage = memoryStorage();
  storage.setItem('cff_roster_transaction_versions', JSON.stringify({ league: 9 }));
  assert.equal(helpers.storedVersion('league', storage), 9);
  assert.equal(helpers.storedVersion('missing', storage), 0);
}

(async () => {
  testVersionOrdering();
  testOperationReuse();
  await testUncertainRetryUsesSameRequest();
  testFailureClassificationAndMessages();
  testStoredVersions();
  console.log('roster transaction frontend tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
