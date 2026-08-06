const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

async function main() {
  const requests = [];
  const listeners = new Map();
  let cleared = false;
  let collectionSyncs = 0;
  const league = { id: 'league-b', name: 'League B' };
  let assignedTeam = true;

  class CustomEvent {
    constructor(type, init = {}) {
      this.type = type;
      this.detail = init.detail;
    }
  }

  const window = {
    getAuthState: () => ({ email: 'member@example.com', token: 'server-token' }),
    isLocalDemoSession: () => false,
    getLeagueState: () => league,
    currentMemberRole: () => 'member',
    isCurrentCommissioner: () => false,
    syncActiveLeagueCollectionsFromApi: async () => {
      collectionSyncs += 1;
      return 'collections';
    },
    syncDraftFromApi: async () => 'draft',
    clearSessionState: () => {
      cleared = true;
    },
    apiRequest: async (requestPath, options = {}) => {
      requests.push({ path: requestPath, options });
      if (/^\/leagues\/league-[bc]\/context$/.test(requestPath)) {
        const requestedLeagueId = requestPath.split('/')[2];
        return {
          leagueId: requestedLeagueId,
          leagueName: requestedLeagueId === 'league-b' ? 'League B' : 'League C',
          userRole: 'COMMISSIONER',
          isCommissioner: true,
          teamAssigned: assignedTeam,
          teamId: assignedTeam ? 'member@example.com' : null,
          teamName: assignedTeam ? 'Mountain Thunder' : '',
          permissions: {
            canEditLineup: assignedTeam,
            canAddPlayers: assignedTeam,
            canProposeTrades: assignedTeam,
            canManageLeague: true
          },
          serverTime: '2026-08-06T20:30:00Z'
        };
      }
      return { ok: true };
    },
    CFF_LEAGUE_CONTEXT: {
      currentLeagueId: () => league.id
    },
    addEventListener(type, listener) {
      listeners.set(type, listener);
    },
    dispatchEvent(event) {
      listeners.get(event.type)?.(event);
      return true;
    },
    setInterval,
    clearInterval
  };
  window.window = window;

  const context = {
    window,
    CustomEvent,
    URL,
    encodeURIComponent,
    decodeURIComponent,
    console,
    setInterval,
    clearInterval
  };
  vm.createContext(context);
  const source = fs.readFileSync(path.join(__dirname, '..', 'league-context-authority.js'), 'utf8');
  vm.runInContext(source, context, { filename: 'league-context-authority.js' });

  const authority = await window.syncLeagueContextFromApi();
  assert.equal(authority.leagueId, 'league-b');
  assert.equal(window.getLeagueContext().teamName, 'Mountain Thunder');
  assert.equal(window.currentMemberRole(league), 'commissioner');
  assert.equal(window.isCurrentCommissioner(league), true);

  const countBeforeMismatch = requests.length;
  await assert.rejects(
    window.apiRequest('/leagues/league-a/roster/player-1/slot', {
      method: 'POST',
      body: '{}'
    }),
    (error) => error.status === 409 && error.data?.code === 'LEAGUE_CONTEXT_MISMATCH'
  );
  assert.equal(requests.length, countBeforeMismatch, 'mismatched mutation reached the network');

  await window.apiRequest('/leagues/league-b/roster/player-1/slot', {
    method: 'POST',
    body: '{}'
  });
  assert.equal(requests.at(-1).path, '/leagues/league-b/roster/player-1/slot');

  const beforeCollections = requests.length;
  assert.equal(await window.syncActiveLeagueCollectionsFromApi(), 'collections');
  assert.equal(collectionSyncs, 1);
  assert.equal(requests.length, beforeCollections, 'cached context should not be fetched twice');

  assignedTeam = false;
  league.id = 'league-c';
  window.dispatchEvent(new CustomEvent('cff:league-context-changed', {
    detail: { leagueId: 'league-c' }
  }));
  await assert.rejects(
    window.apiRequest('/leagues/league-c/waivers', {
      method: 'POST',
      body: '{}'
    }),
    (error) => error.status === 403 && error.data?.code === 'TEAM_ASSIGNMENT_REQUIRED'
  );
  assert.equal(requests.at(-1).path, '/leagues/league-c/context');

  window.clearSessionState();
  assert.equal(cleared, true);
  assert.equal(window.getLeagueContext(), null);

  console.log('league-context-authority.test.js passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
