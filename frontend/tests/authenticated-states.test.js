'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');

const {
  requestMethod,
  requestStateMessage,
  emptyStateDefinition,
  emptyStateTitle
} = require(path.join('..', 'alpha-ui.js'));

assert.equal(requestMethod(), 'GET');
assert.equal(requestMethod({ method: 'post' }), 'POST');
assert.equal(requestMethod({ method: ' Patch ' }), 'PATCH');

assert.equal(requestStateMessage('/leagues/league-1/draft/order', 'PUT'), 'Draft order saved.');
assert.equal(requestStateMessage('/leagues/league-1/waivers', 'POST'), 'Waiver changes saved.');
assert.equal(requestStateMessage('/leagues/league-1/trades', 'POST'), 'Trade changes saved.');
assert.equal(requestStateMessage('/leagues/league-1/roster/drop', 'POST'), 'Roster changes saved.');
assert.equal(requestStateMessage('/leagues/league-1', 'PUT'), 'League changes saved.');
assert.equal(requestStateMessage('/leagues', 'GET'), 'Latest data loaded.');

const leagueEmpty = emptyStateDefinition('league-empty');
assert.deepEqual(leagueEmpty, {
  title: 'No active league',
  body: 'Create or join a league to unlock rosters, matchups, waivers, trades, and the draft room.',
  actionLabel: 'Create league',
  actionHref: 'index.html'
});
leagueEmpty.title = 'Changed by test';
assert.equal(emptyStateDefinition('league-empty').title, 'No active league', 'definitions must be returned as copies');

assert.equal(emptyStateDefinition('missing-state'), null);
assert.equal(emptyStateTitle('draft-pick-list', 'No picks made yet.'), 'No picks made');
assert.equal(emptyStateTitle('upcoming-pick-list', 'Draft complete.'), 'Draft complete');
assert.equal(emptyStateTitle('missing-state', ''), 'Nothing here yet');

console.log('authenticated page state helper tests passed');
