'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const leagueSource = fs.readFileSync(path.join(__dirname, '..', 'league.js'), 'utf8');
const leagueHtml = fs.readFileSync(path.join(__dirname, '..', 'league.html'), 'utf8');
const draftHtml = fs.readFileSync(path.join(__dirname, '..', 'draft.html'), 'utf8');

assert.match(
  leagueSource,
  /function findAvailablePlayerById\(playerId\)/,
  'league actions must resolve players from the rendered available-player pool'
);

assert.match(
  leagueSource,
  /const player = findAvailablePlayerById\(button\.dataset\.addFreeAgent\);/,
  'free-agent add buttons must support real server player ids, not only demo ids'
);

assert.match(
  leagueSource,
  /const player = findAvailablePlayerById\(waiverAddPlayer\.value\);/,
  'waiver claims must submit the selected real server player object'
);

assert.ok(
  leagueHtml.indexOf('state.js') < leagueHtml.indexOf('league-beta-stability.js')
    && leagueHtml.indexOf('league-beta-stability.js') < leagueHtml.indexOf('league.js'),
  'league stability layer must load after state.js and before league.js'
);

assert.ok(
  draftHtml.indexOf('state.js') < draftHtml.indexOf('league-beta-stability.js')
    && draftHtml.indexOf('league-beta-stability.js') < draftHtml.indexOf('draft.js'),
  'draft stability layer must load after state.js and before draft.js'
);

console.log('league real-player action contracts passed');
