(function initCffApiClient(root) {
  'use strict';

  const DEFAULT_TIMEOUT_MS = 12000;
  const DEFAULT_MAX_RETRIES = 2;
  const MAX_RETRY_DELAY_MS = 5000;
  const SAFE_RETRY_METHODS = new Set(['GET', 'HEAD', 'OPTIONS']);
  const RETRYABLE_STATUSES = new Set([408, 425, 502, 503, 504]);

  class CFFApiError extends Error {
    constructor(message, details = {}) {
      super(message);
      this.name = 'CFFApiError';
      Object.assign(this, details);
    }
  }

  function methodName(value = 'GET') {
    return String(value || 'GET').trim().toUpperCase() || 'GET';
  }

  function clampInteger(value, fallback, minimum, maximum) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return fallback;
    return Math.min(maximum, Math.max(minimum, Math.trunc(numeric)));
  }

  function createRequestId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') {
      return cryptoObject.randomUUID();
    }
    const timestamp = Math.max(0, Number(now()) || 0).toString(36);
    const entropy = Math.floor(Math.max(0, Number(random()) || 0) * Number.MAX_SAFE_INTEGER).toString(36);
    return `cff-${timestamp}-${entropy}`.slice(0, 96);
  }

  function parseRetryAfter(value, nowMs = Date.now()) {
    const raw = String(value || '').trim();
    if (!raw) return null;
    if (/^\d+$/.test(raw)) {
      return Math.max(0, Number(raw) * 1000);
    }
    const parsed = Date.parse(raw);
    if (!Number.isFinite(parsed)) return null;
    return Math.max(0, parsed - nowMs);
  }

  function retryDelayMs(attempt, retryAfterMs = null, random = Math.random) {
    if (Number.isFinite(retryAfterMs)) {
      return Math.min(MAX_RETRY_DELAY_MS, Math.max(0, retryAfterMs));
    }
    const base = Math.min(2000, 250 * (2 ** Math.max(0, Number(attempt) || 0)));
    const jitter = 0.75 + Math.max(0, Math.min(1, Number(random()) || 0)) * 0.5;
    return Math.round(base * jitter);
  }

  function shouldRetry({
    method = 'GET',
    status = 0,
    error = null,
    attempt = 0,
    maxRetries = DEFAULT_MAX_RETRIES,
    retryAfterMs = null
  } = {}) {
    if (!SAFE_RETRY_METHODS.has(methodName(method))) return false;
    if (attempt >= maxRetries) return false;
    if (error) {
      if (error.externalAborted || error.aborted) return false;
      return Boolean(error.timedOut || error.unavailable || !error.status);
    }
    const numericStatus = Number(status || 0);
    if (RETRYABLE_STATUSES.has(numericStatus)) return true;
    return numericStatus === 429
      && Number.isFinite(retryAfterMs)
      && retryAfterMs <= MAX_RETRY_DELAY_MS;
  }

  function requestReference(requestId = '') {
    const value = String(requestId || '').trim();
    return value ? ` Reference: ${value}.` : '';
  }

  function compactDiagnosticValue(value, maximum = 120) {
    const text = String(value || '').replace(/\s+/g, ' ').trim();
    if (!text) return '';
    return text.length > maximum ? `${text.slice(0, maximum - 1)}…` : text;
  }

  function apiErrorDiagnostics(error = {}) {
    const fields = [];
    const code = compactDiagnosticValue(error.code || error.data?.code);
    const status = Number(error.status || 0);
    const method = compactDiagnosticValue(error.method);
    const path = compactDiagnosticValue(error.path);
    const requestId = compactDiagnosticValue(error.requestId || error.correlationId);
    const attempts = Number(error.attempts || 0);
    const retryAfter = compactDiagnosticValue(error.retryAfter);
    if (code) fields.push(`code=${code}`);
    if (status) fields.push(`status=${status}`);
    if (method) fields.push(`method=${method}`);
    if (path) fields.push(`path=${path}`);
    if (attempts > 1) fields.push(`attempts=${attempts}`);
    if (retryAfter) fields.push(`retryAfter=${retryAfter}`);
    if (requestId) fields.push(`requestId=${requestId}`);
    return fields.length ? `Diagnostics: ${fields.join(' ')}` : '';
  }

  function normalizedUserMessage(error, fallback = 'The request could not be completed.') {
    const reference = requestReference(error?.requestId);
    if (error?.timedOut) {
      return `The request timed out. Check your connection and try again.${reference}`;
    }
    if (error?.status === 429) {
      return `Too many requests. Try again later.${reference}`;
    }
    if (error?.status === 401) {
      return `Your session is no longer authorized. Sign in again.${reference}`;
    }
    if (error?.status === 403) {
      return `${error?.data?.error || 'You are not allowed to perform this action.'}${reference}`;
    }
    if (error?.unavailable || [502, 503, 504].includes(Number(error?.status || 0))) {
      return `The service is temporarily unavailable. No unconfirmed changes were saved.${reference}`;
    }
    return `${error?.data?.error || error?.message || fallback}${reference}`;
  }

  function normalizeApiError(error, context = {}) {
    if (error instanceof CFFApiError && !context.requestId) return error;
    const status = Number(error?.status || context.status || 0);
    const data = error?.data && typeof error.data === 'object' ? error.data : null;
    const requestId = String(
      error?.requestId
      || data?.requestId
      || data?.correlationId
      || context.requestId
      || ''
    ).trim();
    const timedOut = Boolean(error?.timedOut || context.timedOut);
    const externalAborted = Boolean(error?.externalAborted || context.externalAborted);
    const aborted = Boolean(externalAborted || error?.aborted);
    const unavailable = Boolean(
      error?.unavailable
      || timedOut
      || (!status && !aborted)
      || [502, 503, 504].includes(status)
    );
    const code = String(data?.code || error?.code || context.code || (
      timedOut ? 'request_timeout' : unavailable ? 'service_unavailable' : status ? `http_${status}` : 'request_failed'
    ));
    const retryAfter = String(error?.retryAfter || context.retryAfter || '');
    const normalized = new CFFApiError(
      data?.error || error?.message || context.fallback || 'Request failed',
      {
        status,
        code,
        data,
        requestId,
        correlationId: requestId,
        retryAfter,
        timedOut,
        aborted,
        externalAborted,
        unavailable,
        retryable: Boolean(context.retryable ?? error?.retryable),
        attempts: Number(context.attempts || error?.attempts || 1),
        method: methodName(context.method || error?.method || 'GET'),
        path: String(context.path || error?.path || '')
      }
    );
    normalized.diagnostics = apiErrorDiagnostics(normalized);
    normalized.userMessage = normalizedUserMessage(normalized, context.fallback);
    normalized.diagnosticMessage = normalized.diagnostics
      ? `${normalized.userMessage} ${normalized.diagnostics}`
      : normalized.userMessage;
    if (error?.stack) normalized.stack = error.stack;
    return normalized;
  }

  function sleep(ms) {
    return new Promise((resolve) => root.setTimeout(resolve, Math.max(0, ms)));
  }

  function apiUrl(input, base = root.CFF_API_BASE || '/api') {
    try {
      const raw = typeof input === 'string' || input instanceof URL ? input : input?.url;
      if (!raw) return null;
      const locationHref = root.location?.href || 'http://localhost/';
      const candidate = new URL(raw, locationHref);
      const configured = new URL(base, locationHref);
      const basePath = configured.pathname.replace(/\/+$/, '') || '/api';
      const sameApi = candidate.origin === configured.origin
        && (candidate.pathname === basePath || candidate.pathname.startsWith(`${basePath}/`));
      return sameApi ? candidate : null;
    } catch {
      return null;
    }
  }

  function createApiFetch(nativeFetch, environment = {}) {
    if (typeof nativeFetch !== 'function') throw new TypeError('A native fetch implementation is required');
    const random = environment.random || Math.random;
    const now = environment.now || Date.now;
    const setTimer = environment.setTimeout || root.setTimeout.bind(root);
    const clearTimer = environment.clearTimeout || root.clearTimeout.bind(root);
    const wait = environment.sleep || sleep;
    const defaultTimeoutMs = clampInteger(environment.defaultTimeoutMs, DEFAULT_TIMEOUT_MS, 100, 120000);
    const defaultMaxRetries = clampInteger(environment.defaultMaxRetries, DEFAULT_MAX_RETRIES, 0, 4);

    return async function cffApiFetch(input, init = {}) {
      const target = apiUrl(input, environment.apiBase || root.CFF_API_BASE || '/api');
      if (!target) return nativeFetch(input, init);

      const requestMethod = methodName(init.method || (typeof Request !== 'undefined' && input instanceof Request ? input.method : 'GET'));
      const timeoutMs = clampInteger(init.cffTimeoutMs, defaultTimeoutMs, 100, 120000);
      const maxRetries = SAFE_RETRY_METHODS.has(requestMethod)
        ? clampInteger(init.cffMaxRetries, defaultMaxRetries, 0, 4)
        : 0;
      const requestId = String(init.cffRequestId || createRequestId(environment.crypto || root.crypto, now, random));
      const externalSignal = init.signal || null;
      const baseHeaders = new Headers(
        typeof Request !== 'undefined' && input instanceof Request ? input.headers : undefined
      );
      new Headers(init.headers || {}).forEach((value, key) => baseHeaders.set(key, value));
      if (!baseHeaders.has('X-Request-ID')) baseHeaders.set('X-Request-ID', requestId);
      if (!baseHeaders.has('Accept')) baseHeaders.set('Accept', 'application/json');

      for (let attempt = 0; attempt <= maxRetries; attempt += 1) {
        const controller = new AbortController();
        let timedOut = false;
        let externalAborted = false;
        const abortFromCaller = () => {
          externalAborted = true;
          controller.abort(externalSignal?.reason);
        };
        if (externalSignal) {
          if (externalSignal.aborted) abortFromCaller();
          else externalSignal.addEventListener('abort', abortFromCaller, { once: true });
        }
        const timeout = setTimer(() => {
          timedOut = true;
          controller.abort();
        }, timeoutMs);

        const requestInit = { ...init, method: requestMethod, headers: baseHeaders, signal: controller.signal };
        delete requestInit.cffTimeoutMs;
        delete requestInit.cffMaxRetries;
        delete requestInit.cffRequestId;

        try {
          const response = await nativeFetch(input, requestInit);
          clearTimer(timeout);
          externalSignal?.removeEventListener?.('abort', abortFromCaller);
          const retryAfterMs = parseRetryAfter(response.headers?.get?.('Retry-After'), now());
          if (shouldRetry({
            method: requestMethod,
            status: response.status,
            attempt,
            maxRetries,
            retryAfterMs
          })) {
            try { await response.body?.cancel?.(); } catch { /* no-op */ }
            await wait(retryDelayMs(attempt, retryAfterMs, random));
            continue;
          }
          try {
            Object.defineProperties(response, {
              cffRequestId: { value: response.headers?.get?.('X-CFF-Request-Id') || requestId },
              cffAttempts: { value: attempt + 1 }
            });
          } catch { /* Response objects may be non-extensible in older browsers. */ }
          return response;
        } catch (error) {
          clearTimer(timeout);
          externalSignal?.removeEventListener?.('abort', abortFromCaller);
          if (externalAborted) {
            error.externalAborted = true;
            error.requestId = error.requestId || requestId;
            error.retryable = false;
            throw error;
          }
          const normalized = normalizeApiError(error, {
            requestId,
            method: requestMethod,
            path: target.pathname,
            timedOut,
            attempts: attempt + 1
          });
          normalized.retryable = shouldRetry({
            method: requestMethod,
            error: normalized,
            attempt,
            maxRetries
          });
          if (!normalized.retryable) throw normalized;
          await wait(retryDelayMs(attempt, null, random));
        }
      }

      throw normalizeApiError(new Error('Retry policy exhausted'), {
        requestId,
        method: requestMethod,
        path: target.pathname,
        attempts: maxRetries + 1,
        code: 'retry_exhausted'
      });
    };
  }

  const helpers = {
    CFFApiError,
    DEFAULT_TIMEOUT_MS,
    DEFAULT_MAX_RETRIES,
    SAFE_RETRY_METHODS,
    RETRYABLE_STATUSES,
    methodName,
    createRequestId,
    parseRetryAfter,
    retryDelayMs,
    shouldRetry,
    normalizeApiError,
    normalizedUserMessage,
    apiErrorDiagnostics,
    apiUrl,
    createApiFetch
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined' || typeof root.fetch !== 'function') return;

  if (!root.fetch.__cffSharedApiTransport) {
    const sharedFetch = createApiFetch(root.fetch.bind(root));
    sharedFetch.__cffSharedApiTransport = true;
    sharedFetch.__cffNativeFetch = root.fetch;
    root.fetch = sharedFetch;
  }

  let attempts = 0;
  let lastApiRequest = null;
  let lastFetchJson = null;
  let lastMutationMessage = null;

  function installApiRequestAdapter() {
    const original = root.apiRequest;
    if (typeof original !== 'function' || original === lastApiRequest || original.__cffSharedApiClient) return false;
    const wrapped = async function sharedApiRequest(path, options = {}) {
      const method = methodName(options.method);
      const requestId = String(options.cffRequestId || createRequestId());
      try {
        return await original.call(this, path, { ...options, cffRequestId: requestId });
      } catch (error) {
        throw normalizeApiError(error, { requestId, method, path });
      }
    };
    wrapped.__cffSharedApiClient = true;
    wrapped.__cffOriginal = original;
    root.apiRequest = wrapped;
    lastApiRequest = wrapped;
    return true;
  }

  function installFetchJsonAdapter() {
    const original = root.fetchJson;
    if (typeof original !== 'function' || original === lastFetchJson || original.__cffSharedApiClient) return false;
    const wrapped = async function sharedFetchJson(url, options = {}, timeoutMs) {
      const method = methodName(options.method);
      const requestId = String(options.cffRequestId || createRequestId());
      try {
        return await original.call(this, url, { ...options, cffRequestId: requestId }, timeoutMs);
      } catch (error) {
        throw normalizeApiError(error, { requestId, method, path: String(url || '') });
      }
    };
    wrapped.__cffSharedApiClient = true;
    wrapped.__cffOriginal = original;
    root.fetchJson = wrapped;
    lastFetchJson = wrapped;
    return true;
  }

  function installMutationMessageAdapter() {
    const original = root.mutationErrorMessage;
    if (typeof original !== 'function' || original === lastMutationMessage || original.__cffSharedApiClient) return false;
    const wrapped = function sharedMutationErrorMessage(error, fallback) {
      const normalized = normalizeApiError(error, { fallback });
      const base = original.call(this, normalized, fallback);
      const reference = requestReference(normalized.requestId);
      return reference && !String(base).includes(normalized.requestId) ? `${base}${reference}` : base;
    };
    wrapped.__cffSharedApiClient = true;
    wrapped.__cffOriginal = original;
    root.mutationErrorMessage = wrapped;
    lastMutationMessage = wrapped;
    return true;
  }

  function installAdapters() {
    attempts += 1;
    installApiRequestAdapter();
    installFetchJsonAdapter();
    installMutationMessageAdapter();
    if (attempts < 200) root.setTimeout(installAdapters, 0);
  }

  root.CFFApiClient = Object.freeze({
    ...helpers,
    installed: true,
    request: (...args) => root.fetch(...args)
  });
  document.documentElement.dataset.cffApiClient = 'true';
  root.setTimeout(installAdapters, 0);
})(typeof window !== 'undefined' ? window : globalThis);