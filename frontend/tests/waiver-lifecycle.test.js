'use strict';

const assert = require('node:assert/strict');
const {
  normalizeVersion,
  shouldApplyState,
  createOperationId,
  operationFor,
  clearOperation,
  uncertainFailure,
  waiverErrorMessage,
  claimFailureMessage,
  claimShowsFailureDetails,
  waiverPanelModel,
  orderedClaimsForPanel,
  applyWaiverPanelState,
  submitAuthoritativeWaiverForm
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
  assert.match(waiverErrorMessage({ data: { code: 'player_inactive' } }), /authoritative player pool/i);
  assert.match(waiverErrorMessage({ data: { code: 'waiver_deadline_passed' } }), /closed/i);
  assert.match(waiverErrorMessage({ data: { code: 'drop_player_locked' } }), /locked/i);
  assert.match(waiverErrorMessage({ timedOut: true }), /same operation/i);
  assert.match(waiverErrorMessage({ data: { code: 'waiver_claim_out_of_order' } }), /priority order/i);
  assert.equal(
    claimFailureMessage({ failureReason: 'Readable server reason', failureCode: 'player_unavailable' }),
    'Readable server reason'
  );
  assert.match(claimFailureMessage({ failureCode: 'player_unavailable' }), /another manager/i);
  assert.equal(claimShowsFailureDetails({ status: 'Failed', failureCode: 'player_unavailable' }), true);
  assert.equal(claimShowsFailureDetails({ status: 'Expired', failureReason: 'Old period' }), true);
  assert.equal(claimShowsFailureDetails({ status: 'Cancelled', failureCode: 'cancelled_by_manager' }), false,
    'cancelled claims must not render the failure fallback');
  assert.equal(claimShowsFailureDetails({ status: 'Successful', failureCode: 'anything' }), false);
}

function testPanelModel() {
  const closed = waiverPanelModel({
    claimsMutable: false,
    canProcess: true,
    periodProcessed: false,
    processingPeriod: 'week-2',
    pendingCount: 2,
    claims: [{ status: 'Pending' }, { status: 'Pending' }]
  });
  assert.equal(closed.claimsMutable, false);
  assert.equal(closed.canProcess, true, 'closed claim window must still allow an eligible commissioner process run');
  assert.equal(closed.pendingCount, 2);
  assert.equal(closed.processingPeriod, 'week-2');

  const completed = waiverPanelModel({ claimsMutable: false, canProcess: true, periodProcessed: true });
  assert.equal(completed.canProcess, false, 'completed period must suppress another award run');
}

function testClaimOrdering() {
  const claims = orderedClaimsForPanel([
    { id: 'b', priority: 2, claimOrder: 1, createdAt: '2026-08-07T01:00:00Z' },
    { id: 'c', priority: 1, claimOrder: 2, createdAt: '2026-08-07T01:00:00Z' },
    { id: 'a', priority: 1, claimOrder: 1, createdAt: '2026-08-07T01:00:00Z' }
  ]);
  assert.deepEqual(claims.map((claim) => claim.id), ['a', 'c', 'b']);
}

function testPanelRepairsLegacyLocksAndRendersFailure() {
  const originalDocument = global.document;
  const addSelect = { disabled: true, options: [{}] };
  const dropSelect = { disabled: true };
  const submitButton = { disabled: true };
  const cancelButton = { disabled: false };
  const upButton = { disabled: false };
  const downButton = { disabled: false };
  let processRow;
  const processAll = { disabled: true, closest: () => processRow };
  const processOne = { disabled: true };
  const processCopy = { textContent: 'Waivers are locked after finalized matchups.' };
  processRow = {
    querySelector(selector) {
      if (selector === '.muted') return processCopy;
      if (selector === '[data-process-all-waivers]') return processAll;
      return null;
    }
  };
  const badge = { textContent: 'Done' };
  const detailChildren = [];
  const claimDetails = { appendChild: (node) => detailChildren.push(node) };
  const claimRow = {
    querySelector(selector) {
      if (selector === '[data-process-all-waivers]') return null;
      if (selector === '.badge') return badge;
      if (selector === '[data-waiver-failure]') return null;
      if (selector === 'div') return claimDetails;
      return null;
    }
  };
  const list = {
    querySelector(selector) {
      if (selector === '[data-process-all-waivers]') return processAll;
      return null;
    },
    querySelectorAll(selector) {
      if (selector === '[data-cancel-waiver]') return [cancelButton];
      if (selector === '[data-waiver-up], [data-waiver-down]') return [upButton, downButton];
      if (selector === '[data-process-all-waivers], [data-process-waiver]') return [processAll, processOne];
      if (selector === '.row') return [processRow, claimRow];
      return [];
    }
  };
  const status = { textContent: 'Waivers are locked after finalized matchups.' };
  const elements = {
    'waiver-add-player': addSelect,
    'waiver-drop-player': dropSelect,
    'waiver-form': { querySelector: () => submitButton },
    'waiver-status': status,
    'waiver-list': list
  };
  global.document = {
    getElementById(id) { return elements[id] || null; },
    createElement() {
      return {
        className: '',
        dataset: {},
        textContent: '',
        setAttribute(name, value) { this[name] = value; }
      };
    }
  };

  try {
    applyWaiverPanelState({
      claimsMutable: false,
      canProcess: true,
      periodProcessed: false,
      pendingCount: 1,
      waiverRules: { claimDeadline: '2026-08-07T12:00:00Z' },
      claims: [{
        id: 'claim-1',
        priority: 1,
        claimOrder: 1,
        createdAt: '2026-08-07T01:00:00Z',
        status: 'Failed',
        failureCode: 'player_unavailable',
        failureReason: 'Player was awarded to another manager.'
      }]
    });
    assert.equal(addSelect.disabled, true, 'closed claim window must block new claims');
    assert.equal(dropSelect.disabled, true, 'closed claim window must freeze conditional-drop edits');
    assert.equal(submitButton.disabled, true, 'closed claim window must block submit');
    assert.equal(cancelButton.disabled, true, 'closed claim window must block cancellation');
    assert.equal(upButton.disabled, true, 'closed claim window must block reordering');
    assert.equal(downButton.disabled, true, 'closed claim window must block reordering');
    assert.equal(processAll.disabled, false, 'server canProcess must override the legacy finalized-matchup lock');
    assert.equal(processOne.disabled, false, 'server canProcess must enable ordered individual processing');
    assert.match(processCopy.textContent, /ready to process/i);
    assert.match(status.textContent, /awaiting processing/i);
    assert.equal(badge.textContent, 'Failed');
    assert.equal(detailChildren.length, 1);
    assert.equal(detailChildren[0].dataset.waiverFailure, 'true');
    assert.equal(detailChildren[0].textContent, 'Player was awarded to another manager.');
  } finally {
    global.document = originalDocument;
  }
}

async function testAuthoritativeSubmitBypassesLegacyFinalizedGuard() {
  const names = [
    'document', 'getAuthState', 'isLocalDemoSession', 'getLeagueState',
    'submitWaiverClaimApi', 'renderLeague', 'lineupLocked'
  ];
  const originals = Object.fromEntries(names.map((name) => [name, global[name]]));
  const addSelect = { value: 'player-late-week' };
  const dropSelect = { value: 'drop-player' };
  const status = { textContent: '' };
  let submitted = null;
  let legacyLockChecks = 0;
  let prevented = false;
  let stopped = false;

  global.document = {
    getElementById(id) {
      if (id === 'waiver-add-player') return addSelect;
      if (id === 'waiver-drop-player') return dropSelect;
      if (id === 'waiver-status') return status;
      return null;
    }
  };
  global.getAuthState = () => ({ token: 'server-token' });
  global.isLocalDemoSession = () => false;
  global.getLeagueState = () => ({ id: 'league-1' });
  global.submitWaiverClaimApi = async (player, dropPlayerId) => {
    submitted = { player, dropPlayerId };
  };
  global.renderLeague = () => {};
  global.lineupLocked = () => {
    legacyLockChecks += 1;
    return true;
  };

  try {
    const handled = await submitAuthoritativeWaiverForm({
      preventDefault() { prevented = true; },
      stopImmediatePropagation() { stopped = true; }
    }, {
      leagueId: 'league-1',
      claimsMutable: true,
      periodProcessed: false,
      claims: []
    });
    assert.equal(handled, true);
    assert.equal(prevented, true, 'authoritative server submit must prevent the legacy form handler');
    assert.equal(stopped, true, 'authoritative server submit must stop the legacy finalized-matchup handler');
    assert.equal(legacyLockChecks, 0, 'Phase 5 submit must not consult the historical lineupLocked guard');
    assert.deepEqual(submitted, {
      player: { id: 'player-late-week', playerId: 'player-late-week' },
      dropPlayerId: 'drop-player'
    });
    assert.equal(status.textContent, 'Waiver claim submitted.');
  } finally {
    for (const name of names) {
      if (typeof originals[name] === 'undefined') delete global[name];
      else global[name] = originals[name];
    }
  }
}

async function main() {
  testVersionContracts();
  testOperationReuse();
  testFallbackOperationId();
  testUncertainFailures();
  testMessages();
  testPanelModel();
  testClaimOrdering();
  testPanelRepairsLegacyLocksAndRendersFailure();
  await testAuthoritativeSubmitBypassesLegacyFinalizedGuard();
  console.log('waiver lifecycle browser contracts passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
