(() => {
  'use strict';

  const API_CACHE_META_KEY = 'cff_api_cache_meta';

  function readJson(key, fallback = null) {
    try {
      const raw = window.localStorage.getItem(key);
      return raw ? JSON.parse(raw) : fallback;
    } catch {
      return fallback;
    }
  }

  function authenticatedServerSessionPresent() {
    try {
      const auth = JSON.parse(window.sessionStorage.getItem('cff_auth') || 'null');
      const token = String(auth?.token || '');
      return Boolean(token && !token.startsWith('local-demo-'));
    } catch {
      return true;
    }
  }

  // Keep league.js usable during a rolling/static deployment where an older
  // state.js can briefly be cached alongside a newer page script.
  if (typeof window.apiCacheMeta !== 'function') {
    window.apiCacheMeta = (scope = 'league') => readJson(API_CACHE_META_KEY, {})?.[scope] || null;
  }

  if (typeof window.mutationControlsDisabled !== 'function') {
    // Fail closed for authenticated league pages until state.js and the
    // reliable synchronization layer have confirmed server connectivity.
    window.mutationControlsDisabled = () => authenticatedServerSessionPresent();
  }

  if (typeof window.mutationErrorMessage !== 'function') {
    window.mutationErrorMessage = (error, fallback = 'Request failed. No local changes were made.') => {
      if (error?.mutationCommitted) {
        return 'The server saved the change, but the latest data could not be refreshed. Refresh before making another change.';
      }
      if (error?.status === 429) {
        const retry = error.retryAfter ? ` Retry after ${error.retryAfter} seconds.` : ' Try again later.';
        return `Too many requests.${retry}`;
      }
      if (error?.status === 503 || error?.unavailable) {
        return 'Service is temporarily unavailable. No local changes were made.';
      }
      return error?.data?.error || error?.message || fallback;
    };
  }

  const expectedApiBase = 'https://college-ff-api.onrender.com/api';
  if (window.CFF_API_BASE && window.CFF_API_BASE !== expectedApiBase) {
    console.warn(
      `Frontend configuration is stale. Loaded API base: ${window.CFF_API_BASE}; expected: ${expectedApiBase}.`
    );
  }
})();
