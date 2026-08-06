'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const frontendRoot = path.join(__dirname, '..');
const extensions = new Set(['.html', '.css', '.js']);
const mojibakePattern = /[\u00c2\u00e2]/;
const failures = [];

function scanDirectory(directory) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const fullPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'tests') continue;
      scanDirectory(fullPath);
      continue;
    }
    if (!extensions.has(path.extname(entry.name))) continue;
    const source = fs.readFileSync(fullPath, 'utf8');
    if (mojibakePattern.test(source)) {
      failures.push(path.relative(frontendRoot, fullPath));
    }
  }
}

scanDirectory(frontendRoot);

assert.deepEqual(failures, [], 'frontend UI text contains likely mojibake markers');
console.log('frontend UI text encoding checks passed');
