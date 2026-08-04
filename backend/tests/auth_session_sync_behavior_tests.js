#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ROOT = path.resolve(__dirname, '..', '..');
const SOURCE = fs.readFileSync(path.join(ROOT, 'frontend', 'auth-session-sync.js'), 'utf8');

async function recoverFrom(responders, options = {}) {
  const session = new Map();
  let messageListener = null;
  let requestCount = 0;

  class MockBroadcastChannel {
    addEventListener(type, listener) {
      if (type === 'message') messageListener = listener;
    }

    postMessage(message) {
      if (message.type !== 'request') return;
      requestCount += 1;
      responders.forEach((auth, index) => {
        setTimeout(() => {
          messageListener?.({
            data: {
              type: 'response',
              requestId: message.requestId,
              auth
            }
          });
        }, index);
      });
    }
  }

  const window = {
    BroadcastChannel: MockBroadcastChannel,
    crypto: { randomUUID: () => 'auth-test-request' },
    sessionStorage: {
      getItem(key) {
        return session.has(key) ? session.get(key) : null;
      },
      setItem(key, value) {
        session.set(key, value);
      }
    },
    setTimeout,
    clearTimeout
  };

  vm.runInNewContext(SOURCE, {
    window,
    console,
    Date,
    JSON,
    Map,
    Math,
    Number,
    Object,
    Promise,
    String
  });

  const recovered = await window.CFFAuthSessionSync.recover(50, options);
  const stored = session.has('cff_auth') ? JSON.parse(session.get('cff_auth')) : null;
  return { recovered, stored, requestCount };
}

(async () => {
  const oneAccount = await recoverFrom([
    { email: 'manager@example.com', token: 'token-one' }
  ]);
  assert.equal(oneAccount.recovered.email, 'manager@example.com');
  assert.equal(oneAccount.stored.token, 'token-one');
  assert.equal(oneAccount.requestCount, 1);

  const sameAccount = await recoverFrom([
    { email: 'Manager@Example.com', token: 'token-old' },
    { email: 'manager@example.com', token: 'token-new' }
  ]);
  assert.equal(String(sameAccount.recovered.email).toLowerCase(), 'manager@example.com');
  assert.ok(sameAccount.stored?.token, 'same-account tabs should recover one valid token');

  const conflictingAccounts = await recoverFrom([
    { email: 'commissioner@example.com', token: 'commissioner-token' },
    { email: 'manager@example.com', token: 'manager-token' }
  ]);
  assert.equal(conflictingAccounts.recovered, null);
  assert.equal(conflictingAccounts.stored, null);

  const expectedAccount = await recoverFrom([
    { email: 'commissioner@example.com', token: 'commissioner-token' },
    { email: 'manager@example.com', token: 'manager-token' }
  ], { expectedEmail: 'manager@example.com' });
  assert.equal(expectedAccount.recovered.email, 'manager@example.com');
  assert.equal(expectedAccount.stored.token, 'manager-token');

  console.log('cross-tab authentication recovery behavior tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
