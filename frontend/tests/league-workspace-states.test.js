const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const source = fs.readFileSync(
  path.resolve(__dirname, '..', 'league-workspace-states.js'),
  'utf8'
);
const authoritySource = fs.readFileSync(
  path.resolve(__dirname, '..', 'league-context-authority.js'),
  'utf8'
);
assert.match(authoritySource, /league-workspace-states\.js/, 'authority layer must load workspace states');

function storage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); }
  };
}

function createEnvironment({
  search = '',
  auth = { email: 'manager@example.com', token: 'token' },
  leagues = [{ id: 'league-a', name: 'League A' }],
  context = { leagueId: 'league-a', teamAssigned: true },
  pending = [],
  syncCollections = async () => null,
  syncContext = async () => context
} = {}) {
  const listeners = new Map();
  const localStorage = storage({
    cff_pending_join_requests: JSON.stringify({
      [auth?.email || 'anonymous']: pending
    })
  });
  let currentLeagues = leagues;
  let currentContext = context;
  const window = {
    location: { search },
    localStorage,
    document: {
      readyState: 'complete',
      createElement() { return null; },
      getElementById() { return null; },
      querySelector() { return null; },
      addEventListener() {}
    },
    getAuthState: () => auth,
    getLeaguesForCurrentAccount: () => currentLeagues,
    getLeagueState: () => currentLeagues[0] || null,
    getLeagueContext: () => currentContext,
    isLocalDemoSession: () => false,
    apiCacheMeta: () => null,
    mutationControlsDisabled: () => false,
    syncLeagueContextFromApi: async (...args) => {
      const result = await syncContext(...args);
      currentContext = result;
      return result;
    },
    syncActiveLeagueCollectionsFromApi: syncCollections,
    syncDraftFromApi: async () => null,
    replaceLeaguesForCurrentAccount(next) {
      currentLeagues = next;
    },
    clearSessionState() {},
    addEventListener(type, handler) {
      if (!listeners.has(type)) listeners.set(type, []);
      listeners.get(type).push(handler);
    },
    dispatchEvent(event) {
      (listeners.get(event.type) || []).forEach((handler) => handler(event));
    },
    setInterval(handler) { handler(); return 1; },
    clearInterval() {}
  };
  window.window = window;
  const sandbox = {
    window,
    document: window.document,
    URLSearchParams,
    CustomEvent: class CustomEvent {
      constructor(type, options = {}) {
        this.type = type;
        this.detail = options.detail;
      }
    },
    console,
    Promise,
    setTimeout,
    clearTimeout
  };
  vm.runInNewContext(source, sandbox, { filename: 'league-workspace-states.js' });
  return window;
}

async function run() {
  {
    const window = createEnvironment({ search: '?leagueId=missing' });
    const state = window.CFF_LEAGUE_WORKSPACE_STATE.current();
    assert.strictEqual(state.kind, 'league_unavailable');
    assert.strictEqual(state.reason, 'unauthorized_or_missing');
    assert.strictEqual(state.fallbackLeagueId, 'league-a');
    window.CFF_LEAGUE_WORKSPACE_STATE.dismissRouteWarning();
    assert.strictEqual(window.CFF_LEAGUE_WORKSPACE_STATE.current().kind, 'ready');
  }

  {
    const window = createEnvironment({ leagues: [], context: null });
    assert.strictEqual(window.CFF_LEAGUE_WORKSPACE_STATE.current().kind, 'no_leagues');
  }

  {
    const window = createEnvironment({
      leagues: [],
      context: null,
      pending: [{ id: 'league-pending' }]
    });
    const state = window.CFF_LEAGUE_WORKSPACE_STATE.current();
    assert.strictEqual(state.kind, 'pending_invite');
    assert.strictEqual(state.reason, 'approval_pending');
  }

  {
    const unavailable = Object.assign(new Error('unavailable'), {
      status: 503,
      unavailable: true
    });
    const window = createEnvironment({
      context: null,
      syncCollections: async () => { throw unavailable; }
    });
    await assert.rejects(() => window.syncActiveLeagueCollectionsFromApi(), /unavailable/);
    const state = window.CFF_LEAGUE_WORKSPACE_STATE.current();
    assert.strictEqual(state.kind, 'service_failure');
    assert.strictEqual(state.retryable, true);
  }

  {
    const missing = Object.assign(new Error('missing'), { status: 404 });
    const window = createEnvironment({
      context: null,
      syncContext: async () => { throw missing; }
    });
    await window.CFF_LEAGUE_WORKSPACE_STATE.retry();
    const state = window.CFF_LEAGUE_WORKSPACE_STATE.current();
    assert.strictEqual(state.kind, 'league_unavailable');
    assert.strictEqual(state.reason, 'deleted_or_access_removed');
  }

  {
    const window = createEnvironment({ auth: null, leagues: [], context: null, search: '?invite=CODE' });
    assert.strictEqual(window.CFF_LEAGUE_WORKSPACE_STATE.current().kind, 'pending_invite');
  }

  console.log('league workspace state tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});