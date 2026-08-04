(function initMutationConsistency(root) {
  'use strict';

  const MUTATION_METHODS = new Set(['POST', 'PUT', 'PATCH', 'DELETE']);
  const DATA_REVISION_KEY = 'cff_data_revision';
  const CACHE_META_KEY = 'cff_api_cache_meta';
  const SCOPED_CACHE_KEYS = Object.freeze([
    'cff_waivers_by_league',
    'cff_waiver_priorities_by_league',
    'cff_trades_by_league',
    'cff_transactions_by_league',
    'cff_matchups_by_league',
    'cff_draft_picks_by_league',
    'cff_draft_meta_by_league',
    'cff_league_feed_posts_by_league'
  ]);
  const ACTIVE_CACHE_KEYS = Object.freeze([
    'cff_draft_queue',
    'cff_roster'
  ]);

  function methodName(value = 'GET') {
    return String(value || 'GET').trim().toUpperCase() || 'GET';
  }

  function normalizePath(path = '') {
    const raw = String(path || '').trim();
    if (!raw) return '';
    try {
      return new URL(raw, root.location?.href || 'http://localhost/').pathname
        .replace(/^\/api(?=\/|$)/, '') || '/';
    } catch {
      return raw.split('?')[0].replace(/^\/api(?=\/|$)/, '') || '/';
    }
  }

  function leagueIdFromPath(path = '') {
    const match = normalizePath(path).match(/^\/leagues\/([^/]+)/);
    return match ? decodeURIComponent(match[1]) : '';
  }

  function requestPolicy(path = '', method = 'GET') {
    const normalizedMethod = methodName(method);
    const normalizedPath = normalizePath(path);
    if (!MUTATION_METHODS.has(normalizedMethod) || !normalizedPath.startsWith('/leagues')) return null;

    if (normalizedPath === '/leagues' && normalizedMethod === 'POST') {
      return {
        key: 'create-league',
        scopes: ['leagues', 'league', 'draft'],
        activateResultLeague: true,
        message: 'League created and refreshed.'
      };
    }

    if (/^\/leagues\/[^/]+$/.test(normalizedPath)) {
      if (normalizedMethod === 'DELETE') {
        return {
          key: 'delete-league',
          scopes: ['leagues', 'league', 'draft'],
          purgeLeagueId: leagueIdFromPath(normalizedPath),
          message: 'League removed and remaining data refreshed.'
        };
      }
      return {
        key: 'league-settings',
        scopes: ['leagues', 'league', 'draft'],
        message: 'League settings saved and refreshed.'
      };
    }

    if (/\/join$/.test(normalizedPath)) {
      return {
        key: 'join-league',
        scopes: ['leagues', 'league', 'draft'],
        activateResultLeague: true,
        message: 'League membership refreshed.'
      };
    }

    if (/\/members(?:\/|$)/.test(normalizedPath)) {
      return {
        key: 'league-members',
        scopes: ['leagues', 'league', 'draft'],
        message: 'League membership refreshed.'
      };
    }

    if (/\/draft(?:\/|$)/.test(normalizedPath)) {
      const queueOnly = /\/draft\/queue$/.test(normalizedPath);
      return {
        key: queueOnly ? 'draft-queue' : 'draft-state',
        scopes: queueOnly ? ['draft'] : ['draft', 'league'],
        message: queueOnly ? 'Draft queue saved and refreshed.' : 'Draft state saved and refreshed.'
      };
    }

    if (/\/(?:roster|rosters)(?:\/|$)/.test(normalizedPath)) {
      return {
        key: 'roster',
        scopes: ['league'],
        message: 'Roster saved and refreshed.'
      };
    }

    if (/\/(?:waivers|waiver-priority)(?:\/|$)/.test(normalizedPath)) {
      return {
        key: 'waivers',
        scopes: ['league'],
        message: 'Waiver data saved and refreshed.'
      };
    }

    if (/\/trades(?:\/|$)/.test(normalizedPath)) {
      return {
        key: 'trades',
        scopes: ['league'],
        message: 'Trade data saved and refreshed.'
      };
    }

    if (/\/(?:score|matchups)(?:\/|$)/.test(normalizedPath)) {
      return {
        key: 'scoring',
        scopes: ['league'],
        message: 'Schedule and scoring data refreshed.'
      };
    }

    if (/\/feed(?:\/|$)/.test(normalizedPath)) {
      return {
        key: 'league-feed',
        scopes: ['league'],
        message: 'League activity refreshed.'
      };
    }

    return {
      key: 'league-data',
      scopes: ['league'],
      message: 'League data saved and refreshed.'
    };
  }

  function readJson(storage, key, fallback = null) {
    try {
      const raw = storage?.getItem?.(key);
      return raw ? JSON.parse(raw) : fallback;
    } catch {
      return fallback;
    }
  }

  function writeJson(storage, key, value) {
    storage?.setItem?.(key, JSON.stringify(value));
  }

  function uniqueScopes(scopes = []) {
    return [...new Set((Array.isArray(scopes) ? scopes : []).filter(Boolean))];
  }

  function markScopesStale(storage, scopes = [], context = {}) {
    const meta = readJson(storage, CACHE_META_KEY, {}) || {};
    const now = context.now || new Date().toISOString();
    uniqueScopes(scopes).forEach((scope) => {
      const current = meta[scope] || {
        schemaVersion: 1,
        source: 'api',
        fetchedAt: '',
        leagueId: context.leagueId || ''
      };
      meta[scope] = {
        ...current,
        stale: true,
        invalidatedAt: now,
        invalidatedBy: context.mutationId || '',
        leagueId: context.leagueId || current.leagueId || ''
      };
    });
    writeJson(storage, CACHE_META_KEY, meta);
    return meta;
  }

  function purgeLeagueCaches(storage, leagueId = '') {
    const normalizedId = String(leagueId || '');
    if (!normalizedId) return;
    SCOPED_CACHE_KEYS.forEach((key) => {
      const store = readJson(storage, key, {}) || {};
      if (!Object.prototype.hasOwnProperty.call(store, normalizedId)) return;
      delete store[normalizedId];
      writeJson(storage, key, store);
    });
  }

  function clearActiveCaches(storage) {
    ACTIVE_CACHE_KEYS.forEach((key) => storage?.removeItem?.(key));
  }

  function resultLeagueId(result = null) {
    if (!result || typeof result !== 'object') return '';
    if (result.joinStatus === 'pending_approval') return '';
    return String(result.id || result.league?.id || '');
  }

  function isServerSession(rootObject) {
    const auth = typeof rootObject.getAuthState === 'function' ? rootObject.getAuthState() : null;
    const token = String(auth?.token || '');
    return Boolean(token && !token.startsWith('local-demo-'));
  }

  function revisionPayload(status, policy, context = {}) {
    return {
      id: context.mutationId || `mutation-${Date.now().toString(36)}`,
      status,
      policy: policy?.key || 'league-data',
      scopes: uniqueScopes(policy?.scopes || []),
      leagueId: context.leagueId || '',
      at: new Date().toISOString()
    };
  }

  function renderCurrentViews(rootObject) {
    rootObject.renderLeague?.();
    rootObject.renderDraft?.();
    rootObject.renderQueue?.();
    rootObject.renderDraftQueuePreview?.();
  }

  function createCoordinator(rootObject, environment = {}) {
    const storage = environment.storage || rootObject.localStorage;
    const schedule = environment.setTimeout || rootObject.setTimeout?.bind(rootObject) || setTimeout;
    const createId = environment.createId || (() => rootObject.CFFApiClient?.createRequestId?.() || `mutation-${Date.now().toString(36)}`);
    const inFlightRefreshes = new Map();
    let lastFailure = null;

    function publish(status, policy, context = {}) {
      const payload = revisionPayload(status, policy, context);
      writeJson(storage, DATA_REVISION_KEY, payload);
      try {
        rootObject.dispatchEvent?.(new CustomEvent('cff:data-consistency', { detail: payload }));
      } catch {
        // CustomEvent may be unavailable in focused runtime tests.
      }
      return payload;
    }

    async function refreshScopes(scopes = [], context = {}) {
      const requested = uniqueScopes(scopes);
      const key = `${requested.slice().sort().join(',')}|${context.activateLeagueId || ''}`;
      if (inFlightRefreshes.has(key)) return inFlightRefreshes.get(key);

      const refresh = (async () => {
        if (requested.includes('leagues') && typeof rootObject.syncLeaguesFromApi === 'function') {
          await rootObject.syncLeaguesFromApi();
        }

        if (context.activateLeagueId && typeof rootObject.setActiveLeague === 'function') {
          rootObject.setActiveLeague(context.activateLeagueId);
        }

        const activeLeague = rootObject.getLeagueState?.();
        if (!activeLeague?.id) {
          clearActiveCaches(storage);
          return { scopes: requested, leagueId: '' };
        }

        if (requested.includes('league') && typeof rootObject.syncActiveLeagueCollectionsFromApi === 'function') {
          await rootObject.syncActiveLeagueCollectionsFromApi();
        }
        if (requested.includes('draft') && typeof rootObject.syncDraftFromApi === 'function') {
          await rootObject.syncDraftFromApi();
          rootObject.writeApiCacheMeta?.('draft', activeLeague.id);
        }
        return { scopes: requested, leagueId: activeLeague.id };
      })().finally(() => inFlightRefreshes.delete(key));

      inFlightRefreshes.set(key, refresh);
      return refresh;
    }

    function showRefreshFailure(policy, context, error) {
      lastFailure = { policy, context, error };
      schedule(() => {
        rootObject.CFFAsyncStates?.show?.(
          'warning',
          'Change saved; refresh incomplete',
          `The server accepted the change, but the latest data could not be reloaded. Cached ${uniqueScopes(policy.scopes).join(' and ')} data was invalidated.`,
          {
            label: 'Retry refresh',
            once: true,
            onClick: async () => {
              try {
                await refreshScopes(policy.scopes, context);
                publish('refreshed', policy, context);
                lastFailure = null;
                rootObject.CFFAsyncStates?.show?.('success', policy.message, 'The latest server data is displayed.', null, 2200);
                renderCurrentViews(rootObject);
              } catch (retryError) {
                showRefreshFailure(policy, context, retryError);
              }
            }
          }
        );
        renderCurrentViews(rootObject);
      }, 0);
    }

    async function refreshAfterMutation(policy, result, context = {}) {
      const activeBefore = rootObject.getLeagueState?.()?.id || '';
      const activateLeagueId = policy.activateResultLeague ? resultLeagueId(result) : '';
      const leagueId = activateLeagueId || activeBefore || policy.purgeLeagueId || '';
      const mutationContext = {
        ...context,
        mutationId: context.mutationId || createId(),
        activateLeagueId,
        leagueId
      };

      markScopesStale(storage, policy.scopes, mutationContext);
      if (policy.purgeLeagueId) purgeLeagueCaches(storage, policy.purgeLeagueId);
      publish('invalidated', policy, mutationContext);

      try {
        const refreshed = await refreshScopes(policy.scopes, mutationContext);
        const finalContext = { ...mutationContext, leagueId: refreshed.leagueId || leagueId };
        publish('refreshed', policy, finalContext);
        lastFailure = null;
        renderCurrentViews(rootObject);
        return { refreshed: true, context: finalContext };
      } catch (error) {
        markScopesStale(storage, policy.scopes, mutationContext);
        publish('refresh-failed', policy, mutationContext);
        showRefreshFailure(policy, mutationContext, error);
        return { refreshed: false, context: mutationContext, error };
      }
    }

    function install() {
      const original = rootObject.apiRequest;
      if (typeof original !== 'function') return false;
      if (original.__cffMutationConsistency) return true;

      const wrapped = async function mutationConsistentApiRequest(path, options = {}) {
        const method = methodName(options.method);
        const policy = requestPolicy(path, method);
        const result = await original.call(this, path, options);
        if (!policy || options.cffSkipMutationRefresh || !isServerSession(rootObject)) return result;
        await refreshAfterMutation(policy, result, {
          path: normalizePath(path),
          method,
          mutationId: String(options.cffRequestId || createId())
        });
        return result;
      };
      wrapped.__cffMutationConsistency = true;
      wrapped.__cffOriginal = original;
      rootObject.apiRequest = wrapped;
      return true;
    }

    function handleExternalRevision() {
      renderCurrentViews(rootObject);
    }

    rootObject.addEventListener?.('storage', (event) => {
      if (event.key === DATA_REVISION_KEY) handleExternalRevision();
    });

    return {
      install,
      requestPolicy,
      refreshScopes,
      refreshAfterMutation,
      purgeLeagueCaches: (leagueId) => purgeLeagueCaches(storage, leagueId),
      get lastFailure() { return lastFailure; }
    };
  }

  const helpers = {
    MUTATION_METHODS,
    DATA_REVISION_KEY,
    CACHE_META_KEY,
    SCOPED_CACHE_KEYS,
    ACTIVE_CACHE_KEYS,
    methodName,
    normalizePath,
    leagueIdFromPath,
    requestPolicy,
    markScopesStale,
    purgeLeagueCaches,
    clearActiveCaches,
    resultLeagueId,
    isServerSession,
    createCoordinator
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  const coordinator = createCoordinator(root);
  root.CFFMutationConsistency = coordinator;
  if (!coordinator.install()) {
    root.setTimeout?.(() => coordinator.install(), 0);
  }
  document.documentElement.dataset.cffMutationConsistency = 'true';
})(typeof window !== 'undefined' ? window : globalThis);
