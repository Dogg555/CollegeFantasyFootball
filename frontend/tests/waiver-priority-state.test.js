'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const context = {
  console,
  Date,
  URLSearchParams,
  fetch: async () => ({ ok: true, json: async () => ({}) }),
  window: {
    CFF_API_BASE: '/api',
    CFF_ALLOW_LOCAL_DEMO: false,
    CFF_AUTH_VALIDATE_TIMEOUT_MS: 100,
    clearTimeout,
    location: { hostname: 'localhost' },
    setTimeout
  }
};
context.globalThis = context;
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, '..', 'state.js'), 'utf8'),
  context
);

const priority = context.waiverPriorityFromMembers([
  { email: 'commissioner@example.com', status: 'Active', role: 'commissioner' },
  { email: 'active-upper@example.com', status: 'ACTIVE' },
  { email: 'invited@example.com', status: 'Invited' },
  { email: 'pending@example.com', status: 'Pending' },
  { email: 'removed@example.com', status: 'Removed' }
]);

assert.deepEqual(
  priority.map((item) => [item.managerEmail, item.priority]),
  [
    ['commissioner@example.com', 1],
    ['active-upper@example.com', 2]
  ]
);

console.log('waiver priority state tests passed');
