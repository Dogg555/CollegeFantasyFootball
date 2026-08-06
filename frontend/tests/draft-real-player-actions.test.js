'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const draftSource = fs.readFileSync(path.join(__dirname, '..', 'draft.js'), 'utf8');
const draftHtml = fs.readFileSync(path.join(__dirname, '..', 'draft.html'), 'utf8');

assert.match(
  draftSource,
  /const available = getAvailablePlayers\(\)[\s\S]*\.slice\(0, 8\);/,
  'recommended draft board must use the active available-player pool'
);

assert.match(
  draftSource,
  /const player = getAvailablePlayers\(\)\.find\(\(item\) => String\(item\.id\) === String\(button\.dataset\.queue\)\);/,
  'draft queue buttons must resolve real server player ids'
);

assert.doesNotMatch(
  draftHtml,
  /Fallback pool/,
  'draft UI should not label the signed-in player catalog as a fallback pool'
);

assert.match(
  draftHtml,
  /id="recommended-source-label">Player pool</,
  'recommended board should show a neutral player-pool source label'
);

console.log('draft real-player action contracts passed');
