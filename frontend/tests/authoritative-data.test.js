'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const helpers = require(path.join('..', 'authoritative-data.js'));

assert.equal(helpers.localDemoAllowed({ hostname: 'localhost' }, true), true);
assert.equal(helpers.localDemoAllowed({ hostname: 'example.test' }, true), false);
assert.equal(helpers.explicitLocalDemo({ token: 'local-demo-abc' }, { hostname: '127.0.0.1' }, true), true);
assert.equal(helpers.explicitLocalDemo({ token: 'server-token' }, { hostname: '127.0.0.1' }, true), false);
assert.equal(helpers.requireServerSession({ token: 'server-token' }, false).mode, 'server');
assert.equal(helpers.requireServerSession({ token: 'local-demo-abc' }, true).mode, 'demo');
assert.throws(
  () => helpers.requireServerSession(null, false, 'update a roster'),
  (error) => error.status === 401 && error.authRequired && /No browser-only changes/.test(error.message)
);

assert.deepEqual(
  helpers.normalizeMembersAuthoritatively([], [], { email: 'manager@example.test' }, false),
  [],
  'production membership must never infer commissioner authority from browser auth'
);
assert.deepEqual(
  helpers.normalizeMembersAuthoritatively([], [], { email: 'manager@example.test' }, true),
  [{ email: 'manager@example.test', role: 'commissioner', status: 'Active', teamName: '' }],
  'explicit localhost demo mode may seed its demo commissioner'
);
assert.equal(
  helpers.authoritativeDraftManager({ draftOrder: [], currentManager: '' }),
  '',
  'draft manager must not fall back to the signed-in browser account'
);
assert.equal(helpers.authorizedDraftTurn('', { email: 'manager@example.test' }), false);
assert.equal(helpers.authorizedDraftTurn('manager@example.test', { email: 'manager@example.test' }), true);

for (const name of [
  'saveDraftQueueApi',
  'draftPlayerApi',
  'saveLeagueToApi',
  'addFreeAgentApi',
  'submitWaiverClaimApi',
  'submitTradeOfferApi',
  'updateRosterSlotApi',
  'finalizeWeekApi'
]) {
  assert.ok(helpers.SERVER_FUNCTIONS.includes(name), `${name} must require a server session`);
}
for (const name of ['addPlayerToQueue', 'draftPlayer', 'addFreeAgent', 'submitTradeOffer']) {
  assert.ok(helpers.DEMO_ONLY_MUTATIONS.includes(name), `${name} must be limited to explicit demo mode`);
}

const source = fs.readFileSync(path.join(__dirname, '..', 'authoritative-data.js'), 'utf8');
const domListeners = new Map();
const timers = [];
let auth = null;
let queueApiCalls = 0;
let localQueueMutations = 0;

const context = {
  console,
  Promise,
  Map,
  Set,
  Object,
  Array,
  String,
  Number,
  Boolean,
  Error,
  Date,
  JSON,
  encodeURIComponent,
  CFF_ALLOW_LOCAL_DEMO: false,
  location: { hostname: 'app.example.test', pathname: '/league.html' },
  document: {
    readyState: 'loading',
    documentElement: { dataset: {} },
    addEventListener(type, handler) {
      domListeners.set(type, handler);
    },
    getElementById() {
      return null;
    }
  },
  setTimeout(handler) {
    timers.push(handler);
    return timers.length;
  },
  getAuthState() {
    return auth;
  }
};
context.window = context;
context.globalThis = context;
vm.createContext(context);
vm.runInContext(source, context, { filename: 'authoritative-data.js' });

context.saveDraftQueueApi = async function originalSaveDraftQueue() {
  queueApiCalls += 1;
  return { queue: [] };
};
context.addPlayerToQueue = function originalLocalQueueMutation() {
  localQueueMutations += 1;
  return [];
};
context.normalizeMembers = () => [{ email: 'wrong@example.test', role: 'commissioner', status: 'Active' }];
context.activeLeagueManagers = () => [{ email: 'wrong@example.test' }];
context.getLeagueState = () => ({ id: 'league-1', members: [] });
context.getLeaguesForCurrentAccount = () => [{ id: 'league-1' }];
context.saveLeagueForAccount = (league) => ({ ok: true, league });
context.getDraftMeta = () => ({
  status: 'not_started',
  currentPick: 1,
  draftOrder: [],
  currentManager: 'manager@example.test',
  pickDeadline: 'future'
});
context.draftManagerForPick = () => '';
context.currentDraftManager = () => 'manager@example.test';
context.isMyDraftTurn = () => true;
context.getAvailablePlayers = () => [{ id: 'sample-player' }];
context.generateLocalMatchups = () => [{ id: 'local-matchup' }];
context.generateLocalSeasonSchedule = () => [{ id: 'local-season' }];
context.readJson = () => ({});

domListeners.get('DOMContentLoaded')();
while (timers.length) timers.shift()();

assert.equal(context.CFFAuthoritativeData.installed, true);
assert.equal(context.document.documentElement.dataset.cffAuthoritativeData, 'true');

(async () => {
  await assert.rejects(
    context.saveDraftQueueApi([]),
    (error) => error.status === 401 && error.authoritativeStateRequired
  );
  assert.equal(queueApiCalls, 0, 'missing-token request must not reach legacy local fallback');
  assert.throws(() => context.addPlayerToQueue({ id: 'p1' }), /No browser-only changes/);
  assert.equal(localQueueMutations, 0);
  assert.deepEqual(context.normalizeMembers([], []), []);
  assert.equal(context.currentDraftManager(context.getDraftMeta()), '');
  assert.equal(context.isMyDraftTurn(context.getDraftMeta()), false);
  assert.deepEqual(context.getAvailablePlayers(), []);
  assert.deepEqual(context.generateLocalMatchups(), []);
  assert.deepEqual(context.generateLocalSeasonSchedule(), []);
  assert.equal(context.getLeagueState(), null, 'account cache must be hidden without an authenticated account');
  assert.deepEqual(context.getLeaguesForCurrentAccount(), []);

  auth = { email: 'manager@example.test', token: 'server-token' };
  await context.saveDraftQueueApi([]);
  assert.equal(queueApiCalls, 1, 'production token must use the wrapped API function');
  assert.deepEqual(context.normalizeMembers([], []), []);

  context.CFF_ALLOW_LOCAL_DEMO = true;
  context.location.hostname = 'localhost';
  auth = { email: 'demo@example.test', token: 'local-demo-test' };
  context.addPlayerToQueue({ id: 'p2' });
  assert.equal(localQueueMutations, 1, 'explicit localhost demo mode may use local mutations');
  assert.equal(context.normalizeMembers([], [])[0].role, 'commissioner');

  console.log('authoritative authenticated data tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
