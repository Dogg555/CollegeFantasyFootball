'use strict';

const assert = require('node:assert/strict');
const helpers = require('../trade-lifecycle.js');
const packageHelpers = require('../multi-player-trades.js');

function memoryStorage() {
  const values = new Map();
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); }
  };
}

assert.equal(helpers.normalizeVersion('7'), 7);
assert.equal(helpers.normalizeVersion(-1), 0);
assert.equal(helpers.stateVersion({ version: 4 }), 4);
assert.equal(helpers.shouldApplyState({ version: 5 }, { version: 4 }), false);
assert.equal(helpers.shouldApplyState({ version: 5 }, { version: 5 }), true);
assert.equal(helpers.shouldApplyState(null, { version: 0 }), true);

const storage = memoryStorage();
const first = helpers.operationFor('status', 'league-1', 'trade-1:accepted', storage, () => 'operation-1');
const replay = helpers.operationFor('status', 'league-1', 'trade-1:accepted', storage, () => 'operation-2');
assert.equal(first.operationKey, 'operation-1');
assert.equal(replay.operationKey, 'operation-1');
const different = helpers.operationFor('status', 'league-1', 'trade-1:cancelled', storage, () => 'operation-3');
assert.equal(different.operationKey, 'operation-3');
helpers.clearOperation('status', 'league-1', 'operation-3', storage);
const afterClear = helpers.operationFor('status', 'league-1', 'trade-1:cancelled', storage, () => 'operation-4');
assert.equal(afterClear.operationKey, 'operation-4');

assert.equal(helpers.uncertainFailure({}), true);
assert.equal(helpers.uncertainFailure({ status: 503 }), true);
assert.equal(helpers.uncertainFailure({ status: 409 }), false);
assert.equal(
  helpers.tradeErrorMessage({ data: { code: 'trade_state_conflict' } }),
  'Trade offers changed. The latest server state has been loaded.'
);
assert.equal(
  helpers.tradeErrorMessage({ data: { code: 'trade_player_locked' } }),
  'One of those players is already included in another open trade.'
);
assert.equal(
  helpers.tradeErrorMessage({ data: { code: 'commissioner_required' } }),
  'Only the league commissioner can approve or veto this trade.'
);
assert.match(
  helpers.tradeErrorMessage({ status: 503, retryable: true }),
  /same operation will not run twice/i
);

const duplicateOffer = {
  offerPlayers: [
    { id: 'a', name: 'Alpha' },
    { playerId: 'a', name: 'Alpha duplicate' },
    { id: 'b', name: 'Beta' }
  ],
  requestPlayers: [{ id: 'c', name: 'Charlie' }]
};
assert.deepEqual(packageHelpers.offerPlayers(duplicateOffer).map(packageHelpers.playerId), ['a', 'b']);
assert.deepEqual(packageHelpers.requestPlayers(duplicateOffer).map(packageHelpers.playerId), ['c']);
assert.equal(packageHelpers.packageNames(packageHelpers.offerPlayers(duplicateOffer)), 'Alpha, Beta');

// Phase 6 acceptance matrix: uneven packages are valid in every required direction.
assert.equal(packageHelpers.packageValid(['a'], ['b', 'c']), true, '1-for-2 must be a legal package shape');
assert.equal(packageHelpers.packageValid(['a', 'b'], ['c', 'd', 'e']), true, '2-for-3 must be a legal package shape');
assert.equal(packageHelpers.packageValid(['a', 'b', 'c'], ['d']), true, '3-for-1 must be a legal package shape');
assert.equal(packageHelpers.packageValid(['a'], ['b']), true);
assert.equal(packageHelpers.packageValid(['a', 'b'], ['c']), true);
assert.equal(packageHelpers.packageValid([], ['b']), false);
assert.equal(packageHelpers.packageValid(['a'], ['a']), false);
assert.equal(packageHelpers.packageValid(['a', 'a'], ['b']), false);
assert.equal(packageHelpers.packageValid(Array.from({ length: 21 }, (_, index) => `a-${index}`), ['b']), false);
assert.equal(packageHelpers.uncertainFailure({ status: 409 }), false);
assert.equal(packageHelpers.uncertainFailure({ status: 503 }), true);

// Restoring source players for a counter must override legacy "no unlocked players"
// disabling, but never bypass a real lineup/trade lock or an incomplete package.
assert.equal(packageHelpers.counterPackageReady(false, ['locked-source'], ['locked-target']), true);
assert.equal(packageHelpers.counterPackageReady(true, ['locked-source'], ['locked-target']), false);
assert.equal(packageHelpers.counterPackageReady(false, [], ['locked-target']), false);
assert.equal(packageHelpers.counterPackageReady(false, ['locked-source'], []), false);

// Package idempotency uses the shared trade-lifecycle operation store and survives a retry click.
const packageStorage = memoryStorage();
const fingerprint = packageHelpers.packageFingerprint(
  'create', '', 'manager-b@example.test', ['a', 'b'], ['c', 'd'], 'same package'
);
const reorderedFingerprint = packageHelpers.packageFingerprint(
  'create', '', 'MANAGER-B@example.test', ['b', 'a'], ['d', 'c'], 'same package'
);
assert.equal(fingerprint, reorderedFingerprint, 'package fingerprint must be stable across selection order/case');
const packageFirst = packageHelpers.operationFor(
  'create', 'league-2', fingerprint, packageStorage, () => 'package-operation-1'
);
const packageRetry = packageHelpers.operationFor(
  'create', 'league-2', reorderedFingerprint, packageStorage, () => 'package-operation-2'
);
assert.equal(packageFirst.operationKey, 'package-operation-1');
assert.equal(packageRetry.operationKey, 'package-operation-1', 'uncertain retry must reuse the same operation key');
packageHelpers.clearOperation('create', 'league-2', 'package-operation-1', packageStorage);
const packageAfterConfirmation = packageHelpers.operationFor(
  'create', 'league-2', fingerprint, packageStorage, () => 'package-operation-3'
);
assert.equal(packageAfterConfirmation.operationKey, 'package-operation-3');

console.log('trade lifecycle browser tests passed');
