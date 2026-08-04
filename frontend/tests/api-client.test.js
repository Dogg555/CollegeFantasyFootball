'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');

const {
  CFFApiError,
  DEFAULT_TIMEOUT_MS,
  DEFAULT_MAX_RETRIES,
  methodName,
  createRequestId,
  parseRetryAfter,
  retryDelayMs,
  shouldRetry,
  normalizeApiError,
  apiUrl,
  createApiFetch
} = require(path.join('..', 'api-client.js'));

assert.equal(DEFAULT_TIMEOUT_MS, 12000);
assert.equal(DEFAULT_MAX_RETRIES, 2);
assert.equal(methodName(' post '), 'POST');
assert.match(createRequestId(null, () => 1000, () => 0.25), /^cff-/);
assert.equal(parseRetryAfter('3', 0), 3000);
assert.equal(parseRetryAfter('invalid', 0), null);
assert.equal(retryDelayMs(0, 9000, () => 0.5), 5000, 'Retry-After waits must be capped');
assert.equal(shouldRetry({ method: 'GET', status: 503, attempt: 0, maxRetries: 2 }), true);
assert.equal(shouldRetry({ method: 'POST', status: 503, attempt: 0, maxRetries: 2 }), false);
assert.equal(shouldRetry({ method: 'GET', status: 429, attempt: 0, maxRetries: 2, retryAfterMs: 3000 }), true);
assert.equal(shouldRetry({ method: 'GET', status: 429, attempt: 0, maxRetries: 2, retryAfterMs: 60000 }), false);
assert.equal(apiUrl('/api/leagues', '/api').pathname, '/api/leagues');
assert.equal(apiUrl('/assets/logo.svg', '/api'), null);

const normalized = normalizeApiError(Object.assign(new Error('backend unavailable'), {
  status: 503,
  data: { code: 'database_unavailable', error: 'Database unavailable' },
  requestId: 'req-123'
}), { method: 'GET', path: '/leagues' });
assert.ok(normalized instanceof CFFApiError);
assert.equal(normalized.code, 'database_unavailable');
assert.equal(normalized.requestId, 'req-123');
assert.equal(normalized.correlationId, 'req-123');
assert.equal(normalized.unavailable, true);
assert.match(normalized.userMessage, /Reference: req-123/);

(async () => {
  const attempts = [];
  const delays = [];
  const responses = [
    new Response(JSON.stringify({ error: 'temporary' }), {
      status: 503,
      headers: { 'Content-Type': 'application/json', 'Retry-After': '0' }
    }),
    new Response(JSON.stringify({ ok: true }), {
      status: 200,
      headers: { 'Content-Type': 'application/json', 'X-CFF-Request-Id': 'server-echo' }
    })
  ];
  const getFetch = createApiFetch(async (_input, init) => {
    attempts.push({ method: init.method, requestId: init.headers.get('X-Request-ID') });
    return responses.shift();
  }, {
    apiBase: '/api',
    crypto: { randomUUID: () => 'logical-request-id' },
    random: () => 0.5,
    sleep: async (ms) => { delays.push(ms); }
  });

  const response = await getFetch('/api/leagues');
  assert.equal(response.status, 200);
  assert.equal(response.cffRequestId, 'server-echo');
  assert.equal(response.cffAttempts, 2);
  assert.equal(attempts.length, 2);
  assert.deepEqual(attempts.map((attempt) => attempt.method), ['GET', 'GET']);
  assert.deepEqual(attempts.map((attempt) => attempt.requestId), ['logical-request-id', 'logical-request-id']);
  assert.equal(delays.length, 1);

  let mutationCalls = 0;
  const mutationFetch = createApiFetch(async () => {
    mutationCalls += 1;
    return new Response(JSON.stringify({ error: 'temporary' }), {
      status: 503,
      headers: { 'Content-Type': 'application/json' }
    });
  }, { apiBase: '/api', sleep: async () => {} });
  const mutationResponse = await mutationFetch('/api/leagues', { method: 'POST' });
  assert.equal(mutationResponse.status, 503);
  assert.equal(mutationCalls, 1, 'mutations must never be replayed automatically');

  let networkCalls = 0;
  const networkFetch = createApiFetch(async () => {
    networkCalls += 1;
    if (networkCalls === 1) throw new TypeError('network down');
    return new Response('{}', { status: 200, headers: { 'Content-Type': 'application/json' } });
  }, {
    apiBase: '/api',
    crypto: { randomUUID: () => 'network-request-id' },
    sleep: async () => {}
  });
  assert.equal((await networkFetch('/api/health')).status, 200);
  assert.equal(networkCalls, 2, 'safe reads should retry transient network failures');

  let timeoutCalls = 0;
  const timeoutFetch = createApiFetch((_input, init) => {
    timeoutCalls += 1;
    return new Promise((_resolve, reject) => {
      init.signal.addEventListener('abort', () => {
        const error = new Error('aborted');
        error.name = 'AbortError';
        reject(error);
      }, { once: true });
    });
  }, {
    apiBase: '/api',
    crypto: { randomUUID: () => 'timeout-request-id' },
    defaultTimeoutMs: 5,
    defaultMaxRetries: 0
  });
  await assert.rejects(
    timeoutFetch('/api/leagues'),
    (error) => error instanceof CFFApiError
      && error.timedOut === true
      && error.requestId === 'timeout-request-id'
      && error.attempts === 1
  );
  assert.equal(timeoutCalls, 1);

  let abortCalls = 0;
  const abortController = new AbortController();
  const abortFetch = createApiFetch((_input, init) => {
    abortCalls += 1;
    return new Promise((_resolve, reject) => {
      init.signal.addEventListener('abort', () => {
        const error = new Error('caller cancelled');
        error.name = 'AbortError';
        reject(error);
      }, { once: true });
    });
  }, {
    apiBase: '/api',
    crypto: { randomUUID: () => 'caller-request-id' },
    sleep: async () => {}
  });
  const pending = abortFetch('/api/leagues', { signal: abortController.signal });
  abortController.abort();
  await assert.rejects(
    pending,
    (error) => error.name === 'AbortError'
      && error.externalAborted === true
      && error.retryable === false
      && error.requestId === 'caller-request-id'
  );
  assert.equal(abortCalls, 1, 'caller cancellation must never be retried');

  let assetHeaders = null;
  const passthrough = createApiFetch(async (_input, init) => {
    assetHeaders = init.headers || null;
    return new Response('asset', { status: 200 });
  }, { apiBase: '/api' });
  await passthrough('/assets/logo.svg', { headers: { Accept: 'image/svg+xml' } });
  assert.deepEqual(assetHeaders, { Accept: 'image/svg+xml' }, 'non-API requests must remain untouched');

  console.log('shared API client runtime tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});