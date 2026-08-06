const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const source = fs.readFileSync(path.join(__dirname, '..', 'commissioner-controls.js'), 'utf8');
const context = {
  console,
  Math,
  Date,
  MutationObserver: class { observe() {} },
  document: {},
  window: {
    addEventListener() {},
    setTimeout() {},
    crypto: { randomUUID: () => 'test-operation' }
  }
};
vm.createContext(context);
vm.runInContext(source, context);

const controls = context.window.CFF_COMMISSIONER_CONTROLS;
assert.ok(controls);

assert.deepEqual(
  Array.from(controls.memberBlockers({ rosterPlayers: 2, openTrades: 1, pendingWaivers: 1 })),
  ['rostered players', 'open trades', 'pending waivers']
);

const pendingActions = controls.actionPlan(
  { draftStarted: false, activeManagers: 2, teamCount: 4, actorIsOwner: false },
  { status: 'Pending', owner: false, role: 'member' }
);
assert.equal(pendingActions[0].action, 'approve');
assert.equal(pendingActions[0].disabled, false);
assert.equal(pendingActions[1].action, 'reject');

const lockedApproval = controls.actionPlan(
  { draftStarted: true, activeManagers: 2, teamCount: 4, actorIsOwner: false },
  { status: 'Invited', owner: false, role: 'member' }
);
assert.equal(lockedApproval[0].disabled, true);

const ownerActions = controls.actionPlan(
  { draftStarted: false, actorIsOwner: true },
  { status: 'Active', owner: false, role: 'member', rosterPlayers: 0 }
);
assert.deepEqual(Array.from(ownerActions, (item) => item.action), ['promote', 'transfer', 'remove']);

const blockedRemoval = controls.actionPlan(
  { draftStarted: false, actorIsOwner: false },
  { status: 'Active', owner: false, role: 'member', scheduledMatchups: 1 }
).find((item) => item.action === 'remove');
assert.equal(blockedRemoval.disabled, true);
assert.match(blockedRemoval.reason, /scheduled matchups/);

console.log('commissioner browser contracts passed');
