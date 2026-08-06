'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const STATE_PATH = path.resolve(__dirname, '../state.js');
const stateSource = fs.readFileSync(STATE_PATH, 'utf8');

function createStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem(key) { return values.has(key) ? values.get(key) : null; },
    setItem(key, value) { values.set(key, String(value)); },
    removeItem(key) { values.delete(key); },
    clear() { values.clear(); },
    snapshot() { return Object.fromEntries(values.entries()); }
  };
}

function createContext(fetchImpl, host = 'college-fantasy-football.com') {
  const localStorage = createStorage();
  const sessionStorage = createStorage({
    cff_auth: JSON.stringify({ email: 'tester@example.test', token: 'token-test' })
  });
  const document = {
    getElementById() { return null; },
    querySelectorAll() { return []; }
  };
  const window = {
    CFF_API_BASE: '/api',
    CFF_ALLOW_LOCAL_DEMO: false,
    CFF_AUTH_VALIDATE_TIMEOUT_MS: 35,
    location: { hostname: host, href: '', pathname: '/league.html' },
    localStorage,
    sessionStorage,
    setTimeout,
    clearTimeout
  };
  const context = vm.createContext({
    window,
    document,
    localStorage,
    sessionStorage,
    fetch: fetchImpl,
    AbortController,
    URL,
    URLSearchParams,
    console,
    setTimeout,
    clearTimeout,
    Date,
    JSON,
    Map,
    Set,
    Promise,
    Error,
    Number,
    String,
    Boolean,
    Object,
    Array,
    Math
  });
  vm.runInContext(stateSource, context, { filename: STATE_PATH });
  return { context, sessionStorage };
}

async function testHungValidationTimesOut() {
  const { context, sessionStorage } = createContext((_url, options = {}) => new Promise((_resolve, reject) => {
    options.signal?.addEventListener('abort', () => {
      const error = new Error('Aborted');
      error.name = 'AbortError';
      reject(error);
    }, { once: true });
  }));
  const started = Date.now();
  const result = await Promise.race([
    vm.runInContext('validateAuthSessionResult()', context),
    new Promise((resolve) => setTimeout(() => resolve({ testHarnessTimedOut: true }), 250))
  ]);
  const elapsed = Date.now() - started;

  assert.equal(result.testHarnessTimedOut, undefined,
    'session validation stayed pending instead of timing out');
  assert.equal(result.authenticated, false);
  assert.equal(result.unavailable, true);
  assert.equal(result.expired, false);
  assert.equal(result.timedOut, true);
  assert.ok(elapsed < 220, `timeout took too long: ${elapsed}ms`);
  assert.ok(sessionStorage.getItem('cff_auth'),
    'temporary API outage should preserve the saved session for retry');
}

async function testServiceUnavailablePreservesSession() {
  const response = {
    ok: false,
    status: 503,
    async json() { return { error: 'Authentication service is temporarily unavailable' }; },
    headers: { get() { return ''; } }
  };
  const { context, sessionStorage } = createContext(async () => response);
  const result = await vm.runInContext('validateAuthSessionResult()', context);
  assert.deepEqual(JSON.parse(JSON.stringify(result)), {
    authenticated: false,
    unavailable: true,
    expired: false,
    timedOut: false,
    status: 503,
    message: 'Authentication service is temporarily unavailable'
  });
  assert.ok(sessionStorage.getItem('cff_auth'),
    'temporary 503 outage should preserve the session for retry');
}

async function testNetworkFailurePreservesSession() {
  const { context, sessionStorage } = createContext(async () => {
    throw new TypeError('Failed to fetch');
  });
  const result = await vm.runInContext('validateAuthSessionResult()', context);
  assert.equal(result.authenticated, false);
  assert.equal(result.unavailable, true);
  assert.equal(result.expired, false);
  assert.equal(result.timedOut, false);
  assert.equal(result.status, 0);
  assert.ok(sessionStorage.getItem('cff_auth'),
    'network failure should preserve the session for retry');
}

async function testSuccessfulValidationKeepsSession() {
  const response = {
    ok: true,
    status: 200,
    async json() { return { valid: true, email: 'canonical@example.test' }; },
    headers: { get() { return ''; } }
  };
  const { context, sessionStorage } = createContext(async () => response);
  const result = await vm.runInContext('validateAuthSessionResult()', context);
  assert.equal(result.authenticated, true);
  assert.equal(result.unavailable, false);
  assert.equal(result.expired, false);
  const stored = JSON.parse(sessionStorage.getItem('cff_auth'));
  assert.equal(stored.email, 'canonical@example.test');
  assert.equal(stored.token, 'token-test');
}

async function testUnauthorizedStillClearsSession() {
  const response = {
    ok: false,
    status: 401,
    async json() { return { error: 'Unauthorized' }; },
    headers: { get() { return ''; } }
  };
  const { context, sessionStorage } = createContext(async () => response);
  const result = await vm.runInContext('validateAuthSessionResult()', context);
  assert.deepEqual(JSON.parse(JSON.stringify(result)), {
    authenticated: false,
    unavailable: false,
    expired: true,
    status: 401
  });
  assert.equal(sessionStorage.getItem('cff_auth'), null,
    'rejected session must be cleared');
}

(async () => {
  await testHungValidationTimesOut();
  await testServiceUnavailablePreservesSession();
  await testNetworkFailurePreservesSession();
  await testSuccessfulValidationKeepsSession();
  await testUnauthorizedStillClearsSession();
  console.log('auth validation timeout tests passed');
})().catch((error) => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
