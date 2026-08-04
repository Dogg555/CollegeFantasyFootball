'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class FakeElement {
  constructor() {
    this.dataset = {};
    this.listeners = new Map();
    this.textContent = '';
    this.style = {};
    this.hidden = false;
    this.disabled = false;
  }

  addEventListener(type, listener) {
    const listeners = this.listeners.get(type) || [];
    listeners.push(listener);
    this.listeners.set(type, listeners);
  }
}

function createHarness(postJson) {
  const button = new FakeElement();
  button.textContent = 'Sign out';
  const status = new FakeElement();
  const note = new FakeElement();
  let clearCount = 0;
  let currentAuth = { email: 'user@example.com', token: 'token-session' };

  const window = {
    getAuthState: () => currentAuth,
    postJson,
    clearSessionState: () => {
      clearCount += 1;
      currentAuth = null;
    },
    updateSharedNav: () => {},
    setStatus: (element, message, isError = false) => {
      if (!element) return;
      element.textContent = message;
      element.dataset.error = String(isError);
    },
    describeRequestError: (error, fallback) => error?.status === 503
      ? 'Authentication service unavailable. Secure sign out was not confirmed.'
      : fallback,
    sessionStorage: { removeItem: () => {} },
    localStorage: { removeItem: () => {} }
  };
  const document = {
    getElementById: (id) => ({
      'signout-btn': button,
      'login-status': status,
      'auth-note': note
    })[id] || null
  };

  const source = fs.readFileSync(
    path.join(__dirname, '..', 'logout-reliability.js'),
    'utf8'
  );
  vm.runInNewContext(source, { window, document, console, String, Error });

  return {
    button,
    status,
    note,
    clearCount: () => clearCount,
    click: async () => {
      const listener = button.listeners.get('click')?.[0];
      assert.ok(listener, 'secure logout click handler should be installed');
      const event = {
        prevented: false,
        stopped: false,
        preventDefault() { this.prevented = true; },
        stopImmediatePropagation() { this.stopped = true; }
      };
      await listener(event);
      return event;
    }
  };
}

async function main() {
  const success = createHarness(async (path, body, token) => {
    assert.equal(path, '/auth/logout');
    assert.equal(Object.keys(body).length, 0);
    assert.equal(token, 'token-session');
    return { status: 'ok' };
  });
  const successEvent = await success.click();
  assert.equal(successEvent.prevented, true);
  assert.equal(successEvent.stopped, true, 'legacy logout handler should not run after the secure handler');
  assert.equal(success.clearCount(), 1, 'confirmed server revocation should clear browser session state');
  assert.equal(success.button.hidden, true);
  assert.match(success.status.textContent, /Signed out securely/);
  assert.equal(success.note.textContent, 'Not signed in yet.');

  const unavailable = createHarness(async () => {
    const error = new Error('service unavailable');
    error.status = 503;
    error.unavailable = true;
    throw error;
  });
  await unavailable.click();
  assert.equal(unavailable.clearCount(), 0, 'unconfirmed logout must preserve the token for a retry');
  assert.equal(unavailable.button.hidden, false);
  assert.equal(unavailable.button.disabled, false);
  assert.equal(unavailable.button.textContent, 'Sign out');
  assert.match(unavailable.status.textContent, /not confirmed/i);
  assert.equal(unavailable.status.dataset.error, 'true');

  const alreadyInvalid = createHarness(async () => {
    const error = new Error('Unauthorized');
    error.status = 401;
    throw error;
  });
  await alreadyInvalid.click();
  assert.equal(alreadyInvalid.clearCount(), 1, 'an already-invalid server token should be cleared locally');
  assert.match(alreadyInvalid.status.textContent, /no longer recognizes this session/);

  console.log('secure logout reliability tests passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});