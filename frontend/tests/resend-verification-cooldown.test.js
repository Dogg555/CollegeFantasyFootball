'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class FakeElement {
  constructor() {
    this.dataset = {};
    this.listeners = new Map();
    this.attributes = new Map();
    this.textContent = '';
    this.value = '';
    this.disabled = false;
  }

  addEventListener(type, listener) {
    const listeners = this.listeners.get(type) || [];
    listeners.push(listener);
    this.listeners.set(type, listeners);
  }

  setAttribute(name, value) {
    this.attributes.set(name, String(value));
  }

  removeAttribute(name) {
    this.attributes.delete(name);
  }
}

async function main() {
  const form = new FakeElement();
  const email = new FakeElement();
  const status = new FakeElement();
  const button = new FakeElement();
  button.textContent = 'Send verification';
  form.dataset.cooldownSeconds = '60';
  form.querySelector = (selector) => selector === 'button[type="submit"]' ? button : null;

  const elements = {
    'resend-form': form,
    'resend-email': email,
    'resend-cooldown': status
  };
  const storage = new Map();
  const localStorage = {
    getItem: (key) => storage.has(key) ? storage.get(key) : null,
    setItem: (key, value) => storage.set(key, String(value)),
    removeItem: (key) => storage.delete(key)
  };

  let fetchImpl = async () => ({
    ok: true,
    status: 200,
    headers: { get: () => '' },
    clone: () => ({ json: async () => ({ cooldownSeconds: 90 }) })
  });
  const windowListeners = new Map();
  const window = {
    fetch: (...args) => fetchImpl(...args),
    localStorage,
    setInterval: () => 1,
    clearInterval: () => {},
    addEventListener: (type, listener) => windowListeners.set(type, listener)
  };
  const document = {
    getElementById: (id) => elements[id] || null
  };

  const source = fs.readFileSync(
    path.join(__dirname, '..', 'resend-verification-cooldown.js'),
    'utf8'
  );
  vm.runInNewContext(source, {
    window,
    document,
    console,
    Date,
    JSON,
    Math,
    Number,
    String,
    encodeURIComponent,
    setTimeout,
    clearTimeout
  });

  email.value = ' User@Example.com ';
  await window.fetch('/api/auth/resend-verification', {
    method: 'POST',
    body: JSON.stringify({ email: email.value })
  });

  const key = 'cff_verification_resend_cooldown_v1:user%40example.com';
  assert.ok(Number(storage.get(key)) > Date.now(), 'successful resend should persist a future expiry');
  assert.equal(button.disabled, true, 'successful resend should disable the submit button');
  assert.match(button.textContent, /^Resend in \d+s$/, 'button should show a live countdown');
  assert.match(status.textContent, /request another verification email/, 'cooldown guidance should be visible');

  const submitListener = form.listeners.get('submit')[0];
  const blockedEvent = {
    prevented: false,
    stopped: false,
    preventDefault() { this.prevented = true; },
    stopImmediatePropagation() { this.stopped = true; }
  };
  submitListener(blockedEvent);
  assert.equal(blockedEvent.prevented, true, 'repeat submissions should be prevented');
  assert.equal(blockedEvent.stopped, true, 'repeat submissions should not reach auth.js');

  email.value = 'other@example.com';
  form.listeners.get('input')[0]();
  assert.equal(button.disabled, false, 'cooldown should be scoped to the normalized email address');
  assert.equal(button.textContent, 'Send verification');

  fetchImpl = async () => {
    throw new Error('network unavailable');
  };
  await assert.rejects(
    window.fetch('/api/auth/resend-verification', {
      method: 'POST',
      body: JSON.stringify({ email: 'failed@example.com' })
    }),
    /network unavailable/
  );
  assert.equal(
    storage.has('cff_verification_resend_cooldown_v1:failed%40example.com'),
    false,
    'failed requests must not start a cooldown'
  );

  fetchImpl = async () => ({
    ok: false,
    status: 429,
    headers: { get: (name) => name === 'Retry-After' ? '30' : '' },
    clone: () => ({ json: async () => ({}) })
  });
  email.value = 'limited@example.com';
  await window.fetch('/api/auth/resend-verification', {
    method: 'POST',
    body: JSON.stringify({ email: email.value })
  });
  const limitedExpiry = Number(storage.get('cff_verification_resend_cooldown_v1:limited%40example.com'));
  assert.ok(limitedExpiry > Date.now(), 'rate-limit responses should retain the retry window');
  assert.ok(limitedExpiry <= Date.now() + 31_000, 'Retry-After should control the cooldown duration');

  console.log('verification resend cooldown tests passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
