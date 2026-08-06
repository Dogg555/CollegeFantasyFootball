'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appSource = fs.readFileSync(path.join(__dirname, '..', 'app.js'), 'utf8');

assert.match(
  appSource,
  /catch \{[\s\S]*if \(isLocalDemoSession\(\)\) \{[\s\S]*renderSearchResults\(filterSamplePlayers\(term\), true\);/,
  'home player search may use sample fallback only in explicit local demo sessions'
);

assert.match(
  appSource,
  /searchResultsEl\.textContent = 'The current player database is temporarily unavailable\.';/,
  'home player search should fail closed when the production player catalog is unavailable'
);

console.log('home player search contracts passed');
