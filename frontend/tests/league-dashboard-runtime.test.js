'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const helpers = require('../league-dashboard-hub.js');

const runtimeSource = fs.readFileSync(require.resolve('../league-dashboard-runtime.js'), 'utf8');

function memoryStorage() {
  const values = new Map();
  return {
    getItem: (key) => values.get(key) || null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key),
    dump: () => Object.fromEntries(values)
  };
}

function payload(leagueId, label) {
  return {
    leagueId,
    league: { id: leagueId, name: label },
    nextAction: { label, href: 'league.html#team', detail: `${label} detail` },
    lineup: { status: 'ready', warnings: [], rosterCount: 8 },
    currentMatchup: null,
    waivers: { pendingCount: 0, items: [] },
    trades: { actionRequiredCount: 0, items: [] },
    standings: { leaders: [], myTeam: null },
    activity: [],
    commissionerNotices: [],
    deadlines: [],
    freshness: { source: 'api', generatedAt: '2026-08-06T20:00:00Z', stale: false, partial: false }
  };
}

function createElementFactory(elements) {
  return function createElement() {
    const element = {
      id: '',
      className: '',
      dataset: {},
      hidden: false,
      textContent: '',
      href: '',
      _innerHTML: '',
      set innerHTML(value) {
        this._innerHTML = String(value);
        for (const match of this._innerHTML.matchAll(/id="([^"]+)"/g)) {
          if (!elements.has(match[1])) {
            elements.set(match[1], {
              id: match[1],
              dataset: {},
              hidden: false,
              textContent: '',
              href: '',
              innerHTML: ''
            });
          }
        }
      },
      get innerHTML() {
        return this._innerHTML;
      }
    };
    return element;
  };
}

async function flush(timers) {
  while (timers.length) {
    const timer = timers.shift();
    timer();
    await Promise.resolve();
    await new Promise((resolve) => setImmediate(resolve));
  }
  await Promise.resolve();
  await new Promise((resolve) => setImmediate(resolve));
}

function harness() {
  const elements = new Map();
  const timers = [];
  const windowListeners = new Map();
  const documentListeners = new Map();
  const localStorage = memoryStorage();
  const sessionStorage = memoryStorage();
  const apiQueue = [];
  const apiCalls = [];
  let activeLeagueId = 'league-1';
  let activeTab = 'overview';

  const tabs = {
    insertAdjacentElement(_position, element) {
      elements.set(element.id, element);
    }
  };
  const document = {
    getElementById: (id) => elements.get(id) || null,
    querySelector(selector) {
      if (selector === '.league-tabs') return tabs;
      if (selector === '[data-league-tab].is-active') {
        return { dataset: { leagueTab: activeTab } };
      }
      return null;
    },
    createElement: createElementFactory(elements),
    addEventListener(type, callback) {
      documentListeners.set(type, callback);
    }
  };

  const root = {
    CFFLeagueDashboard: helpers,
    document,
    location: { pathname: '/league.html', hash: '#overview' },
    localStorage,
    sessionStorage,
    CFFApiClient: { normalizedUserMessage: (_error, fallback) => fallback },
    getAuthState: () => ({ token: 'real-token', email: 'owner@example.com' }),
    getLeagueState: () => ({ id: activeLeagueId }),
    setActiveLeague(id) {
      activeLeagueId = id;
      return id;
    },
    validateAuthSessionResult: async () => ({ authenticated: true, unavailable: false }),
    apiRequest: async (path) => {
      apiCalls.push(path);
      const next = apiQueue.shift();
      if (next instanceof Error) throw next;
      return next || payload(activeLeagueId, activeLeagueId);
    },
    addEventListener(type, callback) {
      windowListeners.set(type, callback);
    },
    setTimeout(callback) {
      timers.push(callback);
      return timers.length;
    }
  };

  vm.runInNewContext(runtimeSource, {
    window: root,
    globalThis: root,
    document,
    console,
    Intl,
    Date,
    URLSearchParams,
    encodeURIComponent,
    setImmediate
  }, { filename: 'league-dashboard-runtime.js' });

  return {
    root,
    elements,
    timers,
    windowListeners,
    documentListeners,
    apiQueue,
    apiCalls,
    localStorage,
    sessionStorage,
    setActiveTab(value) { activeTab = value; }
  };
}

async function testLeagueSwitchRefreshesAndClearsOldPresentation() {
  const test = harness();
  test.apiQueue.push(payload('league-1', 'League One'));
  await flush(test.timers);
  assert.equal(test.elements.get('league-dashboard-next-label').textContent, 'League One');

  test.apiQueue.push(payload('league-2', 'League Two'));
  test.root.setActiveLeague('league-2');
  assert.equal(test.elements.get('league-dashboard-next-label').textContent, 'Loading…');
  await flush(test.timers);

  assert.equal(test.elements.get('league-dashboard-next-label').textContent, 'League Two');
  assert.ok(test.apiCalls.some((path) => path.includes('/leagues/league-2/dashboard')));
}

async function testAuthorizationFailureClearsCacheAndValidation() {
  const test = harness();
  test.apiQueue.push(payload('league-1', 'League One'));
  await flush(test.timers);
  assert.ok(helpers.loadCache(test.localStorage, 'owner@example.com', 'league-1'));

  const denied = new Error('not found');
  denied.status = 404;
  test.apiQueue.push(denied);
  await test.root.refreshLeagueDashboard();

  assert.equal(helpers.loadCache(test.localStorage, 'owner@example.com', 'league-1'), null);
  const validations = helpers.readStore(test.sessionStorage, helpers.VALIDATED_KEY, {});
  assert.equal(validations[helpers.cacheScope('owner@example.com', 'league-1')], undefined);
  assert.match(test.elements.get('league-dashboard-state').textContent, /access could not be confirmed/i);
}

async function testInjectedHubFollowsTabVisibility() {
  const test = harness();
  test.apiQueue.push(payload('league-1', 'League One'));
  await flush(test.timers);
  const hub = test.elements.get('league-command-center');
  assert.equal(hub.hidden, false);

  test.setActiveTab('team');
  test.documentListeners.get('click')({
    target: { closest: () => ({ dataset: { leagueTab: 'team' } }) }
  });
  await flush(test.timers);
  assert.equal(hub.hidden, true);

  test.setActiveTab('overview');
  test.documentListeners.get('click')({
    target: { closest: () => ({ dataset: { leagueTab: 'overview' } }) }
  });
  await flush(test.timers);
  assert.equal(hub.hidden, false);
}

(async () => {
  await testLeagueSwitchRefreshesAndClearsOldPresentation();
  await testAuthorizationFailureClearsCacheAndValidation();
  await testInjectedHubFollowsTabVisibility();
  console.log('league dashboard runtime frontend tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
