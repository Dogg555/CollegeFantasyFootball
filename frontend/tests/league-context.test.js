const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '..', 'league-context.js'), 'utf8');

function storage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); },
    dump() { return Object.fromEntries(values); }
  };
}

function makeAnchor(href) {
  let value = href;
  return {
    getAttribute(name) { return name === 'href' ? value : null; },
    setAttribute(name, next) { if (name === 'href') value = next; },
    href() { return value; }
  };
}

function createTab({ sharedLocal, session, href, fallbackId = 'league-a', anchors = [] }) {
  const url = new URL(href);
  const leagues = [
    { id: 'league-a', name: 'Alpha' },
    { id: 'league-b', name: 'Bravo' },
    { id: 'league-c', name: 'Charlie' }
  ];
  let legacyActive = fallbackId;
  const document = {
    title: 'League',
    documentElement: {},
    querySelectorAll(selector) { return selector === 'a[href]' ? anchors : []; }
  };
  const location = {
    origin: url.origin,
    href: url.href,
    pathname: url.pathname,
    search: url.search,
    hash: url.hash
  };
  const history = {
    state: {},
    replaceState(state, _title, next) {
      this.state = state;
      const updated = new URL(next, location.origin);
      location.href = updated.href;
      location.pathname = updated.pathname;
      location.search = updated.search;
      location.hash = updated.hash;
    }
  };
  class Event {
    constructor(type, init = {}) { this.type = type; this.detail = init.detail; }
  }
  const window = {
    location,
    history,
    localStorage: sharedLocal,
    sessionStorage: session,
    URL,
    URLSearchParams,
    CustomEvent: Event,
    setInterval,
    clearInterval,
    addEventListener() {},
    dispatchEvent() {},
    getAuthState: () => ({ email: 'manager@example.test', token: 'token-real' }),
    getLeaguesForCurrentAccount: () => leagues,
    getLeagueState: () => leagues.find((league) => league.id === legacyActive) || leagues[0],
    setActiveLeague: (leagueId) => { legacyActive = leagueId; },
    saveLeagueForAccount: (league, options = {}) => ({
      ok: true,
      league,
      activate: options === true || options.activate === true
    }),
    replaceLeaguesForCurrentAccount() {},
    removeLeagueForCurrentAccount(leagueId) {
      if (legacyActive === leagueId) legacyActive = 'league-a';
    },
    clearSessionState() {}
  };
  window.window = window;
  const context = {
    window,
    document,
    URL,
    URLSearchParams,
    CustomEvent: Event,
    MutationObserver: class { observe() {} },
    setInterval,
    clearInterval,
    console
  };
  vm.runInNewContext(source, context, { filename: 'league-context.js' });
  return { window, location, anchors, legacyActive: () => legacyActive };
}

const sharedLocal = storage();

const tabA = createTab({
  sharedLocal,
  session: storage(),
  href: 'https://cff.test/league.html?leagueId=league-a#team'
});
const tabB = createTab({
  sharedLocal,
  session: storage(),
  href: 'https://cff.test/league.html?leagueId=league-b#standings'
});

assert.equal(tabA.window.getLeagueState().id, 'league-a');
assert.equal(tabB.window.getLeagueState().id, 'league-b');

assert.equal(tabA.window.setActiveLeague('league-c'), true);
assert.equal(tabA.window.getLeagueState().id, 'league-c');
assert.match(tabA.location.search, /leagueId=league-c/);
assert.equal(tabB.window.getLeagueState().id, 'league-b', 'another tab must keep its own league context');

const invalid = createTab({
  sharedLocal,
  session: storage(),
  href: 'https://cff.test/draft.html?leagueId=not-a-member',
  fallbackId: 'league-a'
});
assert.equal(invalid.window.getLeagueState().id, 'league-a');
assert.match(invalid.location.search, /leagueId=league-a/);
assert.doesNotMatch(invalid.location.search, /not-a-member/);

const draftLink = makeAnchor('draft.html#queue');
const leagueLink = makeAnchor('league.html?league=league-a#trades');
const externalLink = makeAnchor('https://example.com/league.html');
const linked = createTab({
  sharedLocal,
  session: storage(),
  href: 'https://cff.test/players.html?leagueId=league-b',
  anchors: [draftLink, leagueLink, externalLink]
});
linked.window.CFF_LEAGUE_CONTEXT.decorateLinks();
assert.equal(draftLink.href(), 'draft.html?leagueId=league-b#queue');
assert.equal(leagueLink.href(), 'league.html?leagueId=league-b#trades');
assert.equal(externalLink.href(), 'https://example.com/league.html');

linked.window.clearSessionState();
assert.equal(linked.window.sessionStorage.getItem('cff_active_league_context_by_account'), null);

console.log('league context tests passed');
