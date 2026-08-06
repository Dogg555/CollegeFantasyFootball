'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const navSource = fs.readFileSync(path.join(__dirname, '..', 'league-nav.js'), 'utf8');
const betaSource = fs.readFileSync(path.join(__dirname, '..', 'beta-ui.js'), 'utf8');
const cssSource = fs.readFileSync(path.join(__dirname, '..', 'league-nav.css'), 'utf8');

assert.match(
  betaSource,
  /function setupLeagueMobileNav\(\)[\s\S]*const targets = \[\.\.\.tabs\.querySelectorAll\('\.league-tab'\)\]/,
  'mobile league selector must be generated from all league tabs'
);

assert.match(
  navSource,
  /option\.textContent = `\$\{prefix\} - \$\{tab\.textContent\.trim\(\)\}`;/,
  'mobile league selector labels must use an ASCII separator'
);

assert.match(
  cssSource,
  /@media \(max-width: 760px\)[\s\S]*\.beta-ui \.league-tabs\.league-tabs--grouped \{[\s\S]*display: none;/,
  'grouped tab rail should be hidden on mobile when the selector is shown'
);

console.log('league mobile navigation contracts passed');
