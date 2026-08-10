const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('frontend/draft-phase7.js', 'utf8');
const values = new Map();
const storage = {
  getItem(key) { return values.has(key) ? values.get(key) : null; },
  setItem(key, value) { values.set(key, String(value)); }
};
let id = 0;
const context = {
  console,
  Date,
  Math,
  JSON,
  encodeURIComponent,
  sessionStorage: storage,
  crypto: { randomUUID: () => `operation-${++id}` },
  getLeagueState: () => ({ id: 'league-1', draftDate: '' }),
  isCurrentCommissioner: () => true,
  getAuthState: () => ({ token: 'token-test' }),
  isLocalDemoSession: () => false,
  setInterval: () => 1,
  clearInterval: () => {},
  addEventListener: () => {}
};
context.globalThis = context;
vm.createContext(context);
vm.runInContext(source, context);

const phase7 = context.CFFDraftPhase7;
assert(phase7, 'Phase 7 client API must be exposed');
assert.strictEqual(phase7.lifecycleState({ status: 'not_started', lobbyOpen: false, draftDate: '' }), 'NOT_SCHEDULED');
assert.strictEqual(phase7.lifecycleState({ status: 'not_started', lobbyOpen: false, draftDate: '2026-08-22T18:00:00Z' }), 'SCHEDULED');
assert.strictEqual(phase7.lifecycleState({ status: 'not_started', lobbyOpen: true }), 'LOBBY_OPEN');
assert.strictEqual(phase7.lifecycleState({ status: 'open' }), 'IN_PROGRESS');
assert.strictEqual(phase7.lifecycleState({ status: 'paused' }), 'PAUSED');
assert.strictEqual(phase7.lifecycleState({ status: 'complete' }), 'COMPLETED');
assert.strictEqual(phase7.lifecycleState({ status: 'cancelled' }), 'CANCELLED');
assert.strictEqual(phase7.clockLabel({ status: 'paused', version: 8 }), 'Paused');
assert.strictEqual(phase7.clockLabel({ status: 'complete', version: 9 }), 'Done');
assert.strictEqual(phase7.clockLabel({ status: 'open', version: 10 }), null);

assert.strictEqual(
  phase7.shouldReplaceSnapshot({ status: 'open', version: 4 }, { status: 'paused', version: 5 }),
  false,
  'an older poll must not replace a newer mutation snapshot'
);
assert.strictEqual(
  phase7.shouldReplaceSnapshot({ status: 'open', version: 6 }, { status: 'paused', version: 5 }),
  true,
  'a newer authoritative snapshot must replace the current snapshot'
);
assert.strictEqual(
  phase7.shouldReplaceSnapshot({ status: 'open', revision: 7 }, { status: 'open', version: 7 }),
  true,
  'equal authoritative revisions may refresh equivalent server data'
);

context.CFFDraftLifecycle = {
  latest: () => ({ status: 'open', version: 12, currentPick: 4 }),
  sync: async () => ({ status: 'open', version: 12, currentPick: 4 })
};
assert.strictEqual(
  phase7.currentSnapshot().version,
  12,
  'Phase 7 controls must consume a newer lifecycle-adapter revision before mutating'
);

const first = phase7.operationFor('pause', '{"expectedVersion":3}');
const replay = phase7.operationFor('pause', '{"expectedVersion":3}');
const changed = phase7.operationFor('pause', '{"expectedVersion":4}');
assert.strictEqual(first.operationKey, replay.operationKey, 'same mutation fingerprint must reuse operation key');
assert.notStrictEqual(first.operationKey, changed.operationKey, 'new authoritative version must use a new operation key');
assert.strictEqual(phase7.uncertainFailure({ status: 503 }), true);
assert.strictEqual(phase7.uncertainFailure({ status: 409 }), false);

console.log('draft Phase 7 browser contracts passed');
