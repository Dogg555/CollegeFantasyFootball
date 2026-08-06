(function initReliableSync(root) {
  'use strict';

  const SYNC_STATUS_KEY = 'cff_sync_status';
  const CACHE_META_KEY = 'cff_api_cache_meta';
  const DATA_REVISION_KEY = 'cff_data_revision';
  const MUTATION_METHODS = new Set(['POST', 'PUT', 'PATCH', 'DELETE']);
  const RECENT_COMMIT_TTL_MS = 15000;
  const REFRESH_DEDUPE_MS = 1500;
  const FOCUS_REFRESH_THROTTLE_MS = 30000;

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

  function isServerSession(rootObject = root) {
    const auth = typeof rootObject.getAuthState === 'function' ? rootObject.getAuthState() : null;
    const token = String(auth?.token || '');
    return Boolean(token && !token.startsWith('local-demo-'));
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
    try {
      storage?.setItem?.(key, JSON.stringify(value));
    } catch {
      // Cache metadata must never break a server request.
    }
  }

  function compactError(error = {}) {
    return {
      status: Number(error?.status || 0),
      code: String(error?.code || error?.data?.code || ''),
      requestId: String(error?.requestId || error?.correlationId || ''),
      message: String(error?.data?.error || error?.message || 'Request failed').slice(0, 240),
      unavailable: Boolean(error?.unavailable || error?.timedOut || !error?.status)
    };
  }

  function syncFailure(message, details = {}) {
    const error = new Error(message);
    error.code = 'sync_refresh_failed';
    error.unavailable = true;
    Object.assign(error, details);
    return error;
  }

  function chainContains(fn, marker) {
    const seen = new Set();
    let current = fn;
    while (typeof current === 'function' && !seen.has(current)) {
      if (current[marker]) return true;
      seen.add(current);
      current = current.__cffOriginal;
    }
    return false;
  }

  function resultArray(result, keys = []) {
    if (Array.isArray(result)) return result;
    for (const key of keys) {
      if (Array.isArray(result?.[key])) return result[key];
    }
    return null;
  }

  function defaultResources(rootObject, leagueId) {
    const encoded = encodeURIComponent(leagueId);
    const scoped = (key, value) => rootObject.setLeagueScopedItemsForLeague?.(key, leagueId, value);
    const normalizedRoster = (value) => (value || []).map((player) => (
      typeof rootObject.normalizePlayer === 'function' ? rootObject.normalizePlayer(player) : player
    ));

    return [
      {
        name: 'roster',
        path: `/leagues/${encoded}/roster`,
        apply(value) {
          const roster = resultArray(value, ['roster']);
          if (!roster) throw new Error('Roster response was not an array');
          rootObject.setRoster?.(normalizedRoster(roster));
        }
      },
      {
        name: 'waivers',
        path: `/leagues/${encoded}/waivers`,
        apply(value) {
          const claims = resultArray(value, ['claims', 'waivers']);
          if (!claims) throw new Error('Waiver response was not an array');
          scoped('cff_waivers_by_league', claims);
        }
      },
      {
        name: 'waiverPriority',
        path: `/leagues/${encoded}/waiver-priority`,
        apply(value) {
          const priority = resultArray(value, ['priority']);
          if (!priority) throw new Error('Waiver-priority response was not an array');
          scoped('cff_waiver_priorities_by_league', priority);
        }
      },
      {
        name: 'trades',
        path: `/leagues/${encoded}/trades`,
        apply(value) {
          const offers = resultArray(value, ['offers', 'trades']);
          if (!offers) throw new Error('Trade response was not an array');
          scoped('cff_trades_by_league', offers);
        }
      },
      {
        name: 'transactions',
        path: `/leagues/${encoded}/transactions`,
        apply(value) {
          const transactions = resultArray(value, ['transactions']);
          if (!transactions) throw new Error('Transaction response was not an array');
          scoped('cff_transactions_by_league', transactions);
        }
      },
      {
        name: 'members',
        path: `/leagues/${encoded}/members`,
        apply(value) {
          const members = resultArray(value, ['members']);
          if (!members) throw new Error('Member response was not an array');
          const league = rootObject.getLeagueState?.();
          if (league?.id === leagueId) rootObject.saveLeagueForAccount?.({ ...league, members });
        }
      },
      {
        name: 'matchups',
        path: `/leagues/${encoded}/matchups`,
        apply(value) {
          const matchups = resultArray(value, ['matchups']);
          if (!matchups) throw new Error('Matchup response was not an array');
          scoped('cff_matchups_by_league', matchups);
        }
      }
    ];
  }

  function applyMutationResult(rootObject, path, result) {
    if (!result || typeof result !== 'object') return [];
    const normalizedPath = normalizePath(path);
    const leagueId = leagueIdFromPath(normalizedPath) || rootObject.getLeagueState?.()?.id || '';
    const applied = [];
    const scoped = (key, value) => {
      if (!leagueId || !Array.isArray(value)) return;
      rootObject.setLeagueScopedItemsForLeague?.(key, leagueId, value);
    };
    const normalizeRoster = (value) => value.map((player) => (
      typeof rootObject.normalizePlayer === 'function' ? rootObject.normalizePlayer(player) : player
    ));

    const roster = resultArray(result, ['roster']);
    if (roster && /\/(?:roster|rosters|waivers|trades|draft)(?:\/|$)/.test(normalizedPath)) {
      rootObject.setRoster?.(normalizeRoster(roster));
      applied.push('roster');
    }

    if (/\/draft(?:\/|$)/.test(normalizedPath)
        && typeof rootObject.applyDraftState === 'function'
        && ('status' in result || 'picks' in result || 'queue' in result || 'currentPick' in result)) {
      rootObject.applyDraftState(result);
      applied.push('draft');
    }

    if (/\/(?:waivers|waiver-priority)(?:\/|$)/.test(normalizedPath)) {
      const claims = resultArray(result, ['claims', 'waivers']);
      const priority = resultArray(result?.priority, []) || (Array.isArray(result?.priority) ? result.priority : null);
      if (claims) {
        scoped('cff_waivers_by_league', claims);
        applied.push('waivers');
      }
      if (priority) {
        scoped('cff_waiver_priorities_by_league', priority);
        applied.push('waiverPriority');
      }
    }

    if (/\/trades(?:\/|$)/.test(normalizedPath)) {
      const offers = resultArray(result, ['offers', 'trades']);
      if (offers) {
        scoped('cff_trades_by_league', offers);
        applied.push('trades');
      }
    }

    if (/\/members(?:\/|$)/.test(normalizedPath)) {
      const members = resultArray(result, ['members']);
      const league = rootObject.getLeagueState?.();
      if (members && league?.id === leagueId) {
        rootObject.saveLeagueForAccount?.({ ...league, members });
        applied.push('members');
      }
    }

    if (/\/(?:score|matchups)(?:\/|$)/.test(normalizedPath)) {
      const matchups = resultArray(result, ['matchups']);
      if (matchups) {
        scoped('cff_matchups_by_league', matchups);
        applied.push('matchups');
      }
    }

    if (/^\/leagues\/[^/]+$/.test(normalizedPath)
        && !Array.isArray(result)
        && result.id
        && typeof rootObject.saveLeagueForAccount === 'function') {
      rootObject.saveLeagueForAccount(result);
      applied.push('league');
    }

    return [...new Set(applied)];
  }

  function createCoordinator(rootObject, environment = {}) {
    const storage = environment.storage || rootObject.localStorage;
    const now = environment.now || Date.now;
    const online = environment.online || (() => rootObject.navigator?.onLine !== false);
    const resourcesFactory = environment.resources || ((leagueId) => defaultResources(rootObject, leagueId));
    const generations = new Map();
    const inFlight = new Map();
    const lastRefresh = new Map();
    let recentCommit = null;
    let lastFocusRefreshAt = 0;
    let syncProbeDepth = 0;

    function currentStatus() {
      return readJson(storage, SYNC_STATUS_KEY, {}) || {};
    }

    function publish(patch = {}) {
      const previous = currentStatus();
      const next = {
        schemaVersion: 1,
        health: 'unknown',
        writable: false,
        resources: {},
        ...previous,
        ...patch,
        resources: { ...(previous.resources || {}), ...(patch.resources || {}) },
        updatedAt: new Date(now()).toISOString()
      };
      writeJson(storage, SYNC_STATUS_KEY, next);
      try {
        rootObject.dispatchEvent?.(new CustomEvent('cff:sync-state', { detail: next }));
      } catch {
        // CustomEvent may be unavailable in focused tests.
      }
      return next;
    }

    function updateResources(updates = {}) {
      const current = currentStatus();
      return publish({ resources: { ...(current.resources || {}), ...updates } });
    }

    function activeLeagueId() {
      return String(rootObject.getLeagueState?.()?.id || '');
    }

    function markCacheStale(scope = 'league') {
      if (typeof rootObject.markApiCacheStale === 'function') {
        rootObject.markApiCacheStale(scope);
        return;
      }
      const meta = readJson(storage, CACHE_META_KEY, {}) || {};
      meta[scope] = { ...(meta[scope] || {}), stale: true, invalidatedAt: new Date(now()).toISOString() };
      writeJson(storage, CACHE_META_KEY, meta);
    }

    function markCacheFresh(scope, leagueId = activeLeagueId()) {
      if (typeof rootObject.writeApiCacheMeta === 'function') {
        rootObject.writeApiCacheMeta(scope, leagueId);
      }
    }

    function recordCommittedMutation(context = {}) {
      recentCommit = {
        path: normalizePath(context.path),
        leagueId: String(context.leagueId || leagueIdFromPath(context.path) || activeLeagueId()),
        requestId: String(context.requestId || ''),
        committedAt: Number(context.committedAt || now())
      };
      const status = currentStatus();
      publish({
        health: online()
          ? (status.health === 'healthy' ? 'healthy' : 'recovering')
          : 'offline',
        writable: Boolean(online() && status.writable === true),
        lastMutationCommittedAt: new Date(recentCommit.committedAt).toISOString(),
        lastMutationPath: recentCommit.path,
        lastMutationRequestId: recentCommit.requestId
      });
      try {
        rootObject.dispatchEvent?.(new CustomEvent('cff:mutation-committed', { detail: recentCommit }));
      } catch {
        // no-op
      }
      return recentCommit;
    }

    function hasRecentCommit(leagueId = activeLeagueId()) {
      return Boolean(recentCommit
        && now() - recentCommit.committedAt <= RECENT_COMMIT_TTL_MS
        && (!recentCommit.leagueId || !leagueId || recentCommit.leagueId === leagueId));
    }

    function controlsDisabled() {
      if (!isServerSession(rootObject)) return false;
      if (!online()) return true;
      const status = currentStatus();
      return status.writable !== true || ['offline', 'unavailable'].includes(status.health);
    }

    async function fetchResource(definition) {
      syncProbeDepth += 1;
      try {
        const value = await rootObject.apiRequest(definition.path, {
          method: 'GET',
          cffSkipMutationRefresh: true
        });
        return { definition, value };
      } finally {
        syncProbeDepth -= 1;
      }
    }

    async function runActiveRefresh(options = {}) {
      const leagueId = String(options.leagueId || activeLeagueId());
      if (!isServerSession(rootObject) || !leagueId) return { skipped: true, leagueId, resources: [] };
      const generation = (generations.get(`league:${leagueId}`) || 0) + 1;
      generations.set(`league:${leagueId}`, generation);
      const startedAt = now();
      publish({
        leagueId,
        health: online() ? 'syncing' : 'offline',
        writable: false,
        lastAttemptAt: new Date(startedAt).toISOString()
      });

      if (!online()) {
        markCacheStale('league');
        const error = syncFailure('The device is offline.', { offline: true, leagueId });
        lastRefresh.set(`league:${leagueId}`, { finishedAt: now(), error });
        throw error;
      }

      const definitions = resourcesFactory(leagueId);
      const settled = await Promise.allSettled(definitions.map(fetchResource));
      const currentGeneration = generations.get(`league:${leagueId}`);
      if (currentGeneration !== generation || activeLeagueId() !== leagueId) {
        return { superseded: true, leagueId, resources: [] };
      }

      const resourceUpdates = {};
      const applied = [];
      const failed = [];
      settled.forEach((entry, index) => {
        const definition = definitions[index];
        if (entry.status === 'fulfilled') {
          try {
            definition.apply(entry.value.value);
            applied.push(definition.name);
            resourceUpdates[definition.name] = {
              stale: false,
              lastSuccessAt: new Date(now()).toISOString(),
              error: null
            };
          } catch (error) {
            failed.push({ name: definition.name, error });
            resourceUpdates[definition.name] = {
              stale: true,
              lastFailureAt: new Date(now()).toISOString(),
              error: compactError(error)
            };
          }
        } else {
          failed.push({ name: definition.name, error: entry.reason });
          resourceUpdates[definition.name] = {
            stale: true,
            lastFailureAt: new Date(now()).toISOString(),
            error: compactError(entry.reason)
          };
        }
      });
      updateResources(resourceUpdates);

      if (applied.length) {
        const complete = failed.length === 0;
        if (complete) markCacheFresh('league', leagueId);
        else markCacheStale('league');
        const result = {
          ok: complete,
          partial: !complete,
          leagueId,
          applied,
          failed: failed.map((item) => item.name),
          startedAt,
          finishedAt: now()
        };
        publish({
          leagueId,
          health: complete ? 'healthy' : 'partial',
          writable: complete,
          lastSuccessAt: new Date(now()).toISOString(),
          ...(complete ? { lastFullSuccessAt: new Date(now()).toISOString() } : {})
        });
        if (!complete) {
          rootObject.CFFAsyncStates?.show?.(
            'warning',
            'Some league data is stale',
            `Updated ${applied.length} data source${applied.length === 1 ? '' : 's'}, but ${failed.length} could not be refreshed. Mutation controls remain disabled until every required source refreshes successfully.`,
            null,
            4200
          );
        }
        lastRefresh.set(`league:${leagueId}`, { finishedAt: now(), result });
        return result;
      }

      markCacheStale('league');
      const error = syncFailure('The latest league data could not be loaded.', {
        leagueId,
        failures: failed.map((item) => ({ name: item.name, ...compactError(item.error) }))
      });
      publish({
        leagueId,
        health: 'unavailable',
        writable: false,
        lastFailureAt: new Date(now()).toISOString(),
        lastError: compactError(error)
      });
      lastRefresh.set(`league:${leagueId}`, { finishedAt: now(), error });
      if (options.afterCommittedMutation || hasRecentCommit(leagueId)) {
        error.mutationCommitted = true;
        error.requestId = error.requestId || recentCommit?.requestId || '';
        rootObject.CFFAsyncStates?.show?.(
          'warning',
          'Change saved; refresh incomplete',
          'The server accepted the change, but the latest league data could not be reloaded. Retry refresh before making another change.',
          null,
          5000
        );
        return {
          ok: false,
          refreshFailed: true,
          mutationCommitted: true,
          leagueId,
          applied: [],
          failed: failed.map((item) => item.name),
          error
        };
      }
      throw error;
    }

    async function refreshActiveCollections(options = {}) {
      const leagueId = String(options.leagueId || activeLeagueId());
      const key = `league:${leagueId}`;
      if (!options.force) {
        if (inFlight.has(key)) return inFlight.get(key);
        const recent = lastRefresh.get(key);
        if (recent && now() - recent.finishedAt <= REFRESH_DEDUPE_MS) {
          if (recent.result) return recent.result;
          if (recent.error && (options.afterCommittedMutation || hasRecentCommit(leagueId))) {
            return {
              ok: false,
              refreshFailed: true,
              mutationCommitted: true,
              leagueId,
              applied: [],
              failed: recent.error.failures?.map((item) => item.name) || [],
              error: recent.error,
              reused: true
            };
          }
          if (recent.error) throw recent.error;
        }
      }
      const promise = runActiveRefresh(options).finally(() => inFlight.delete(key));
      inFlight.set(key, promise);
      return promise;
    }

    async function refreshLeagues(options = {}) {
      if (!isServerSession(rootObject)) return rootObject.getLeaguesForCurrentAccount?.() || [];
      const key = 'leagues';
      if (!options.force && inFlight.has(key)) return inFlight.get(key);
      const generation = (generations.get(key) || 0) + 1;
      generations.set(key, generation);
      const request = (async () => {
        try {
          syncProbeDepth += 1;
          let leagues;
          try {
            leagues = await rootObject.apiRequest('/leagues', { method: 'GET', cffSkipMutationRefresh: true });
          } finally {
            syncProbeDepth -= 1;
          }
          if (generations.get(key) !== generation) return { superseded: true };
          rootObject.replaceLeaguesForCurrentAccount?.(Array.isArray(leagues) ? leagues : []);
          markCacheFresh('leagues');
          publish({ health: 'healthy', writable: true, lastSuccessAt: new Date(now()).toISOString() });
          return rootObject.getLeaguesForCurrentAccount?.() || [];
        } catch (error) {
          markCacheStale('leagues');
          publish({ health: online() ? 'unavailable' : 'offline', writable: false, lastError: compactError(error) });
          throw error;
        }
      })().finally(() => inFlight.delete(key));
      inFlight.set(key, request);
      return request;
    }

    async function refreshDraft(options = {}) {
      const leagueId = String(options.leagueId || activeLeagueId());
      if (!isServerSession(rootObject) || !leagueId) return null;
      const key = `draft:${leagueId}`;
      if (!options.force && inFlight.has(key)) return inFlight.get(key);
      const generation = (generations.get(key) || 0) + 1;
      generations.set(key, generation);
      const request = (async () => {
        try {
          syncProbeDepth += 1;
          let state;
          try {
            state = await rootObject.apiRequest(`/leagues/${encodeURIComponent(leagueId)}/draft`, {
              method: 'GET',
              cffSkipMutationRefresh: true
            });
          } finally {
            syncProbeDepth -= 1;
          }
          if (generations.get(key) !== generation || activeLeagueId() !== leagueId) return { superseded: true };
          rootObject.applyDraftState?.(state || {});
          markCacheFresh('draft', leagueId);
          publish({ health: 'healthy', writable: true, lastSuccessAt: new Date(now()).toISOString() });
          return state;
        } catch (error) {
          markCacheStale('draft');
          throw error;
        }
      })().finally(() => inFlight.delete(key));
      inFlight.set(key, request);
      return request;
    }

    async function refreshAll(options = {}) {
      await refreshLeagues(options);
      const leagueId = activeLeagueId();
      if (!leagueId) return { leagueId: '', league: null, draft: null };
      const [league, draft] = await Promise.allSettled([
        refreshActiveCollections({ ...options, leagueId }),
        refreshDraft({ ...options, leagueId })
      ]);
      if (league.status === 'rejected' && draft.status === 'rejected') throw league.reason;
      return {
        leagueId,
        league: league.status === 'fulfilled' ? league.value : null,
        draft: draft.status === 'fulfilled' ? draft.value : null,
        partial: league.status === 'rejected' || draft.status === 'rejected'
      };
    }

    function installApiWrapper() {
      const original = rootObject.apiRequest;
      if (typeof original !== 'function' || chainContains(original, '__cffReliableSync')) return false;
      const wrapped = async function reliableApiRequest(path, options = {}) {
        const method = methodName(options.method);
        try {
          const result = await original.call(this, path, options);
          if (MUTATION_METHODS.has(method) && isServerSession(rootObject)) {
            const applied = applyMutationResult(rootObject, path, result);
            recordCommittedMutation({
              path,
              leagueId: leagueIdFromPath(path) || activeLeagueId(),
              requestId: options.cffRequestId || result?.requestId || result?.correlationId || ''
            });
            if (applied.length && currentStatus().writable === true) {
              publish({ health: 'healthy', writable: true });
            }
          } else if (syncProbeDepth === 0
              && isServerSession(rootObject)
              && normalizePath(path).startsWith('/leagues')) {
            const status = currentStatus();
            if (status.writable !== true || ['unknown', 'recovering'].includes(status.health)) {
              publish({ health: 'healthy', writable: true, lastSuccessAt: new Date(now()).toISOString() });
            }
          }
          return result;
        } catch (error) {
          if (MUTATION_METHODS.has(method) && isServerSession(rootObject)
              && (error?.unavailable || error?.timedOut || !error?.status)) {
            publish({
              health: online() ? 'unavailable' : 'offline',
              writable: false,
              lastFailureAt: new Date(now()).toISOString(),
              lastError: compactError(error)
            });
          }
          throw error;
        }
      };
      wrapped.__cffReliableSync = true;
      wrapped.__cffOriginal = original;
      rootObject.apiRequest = wrapped;
      return true;
    }

    function installSyncFunctions() {
      if (typeof rootObject.getLeagueState !== 'function' || typeof rootObject.apiRequest !== 'function') return false;
      rootObject.syncLeaguesFromApi = refreshLeagues;
      rootObject.syncActiveLeagueCollectionsFromApi = refreshActiveCollections;
      rootObject.syncDraftFromApi = refreshDraft;
      rootObject.mutationControlsDisabled = controlsDisabled;
      return true;
    }

    function installEvents() {
      if (rootObject.__cffReliableSyncEvents) return;
      rootObject.__cffReliableSyncEvents = true;
      rootObject.addEventListener?.('offline', () => {
        publish({ health: 'offline', writable: false });
        markCacheStale('league');
      });
      rootObject.addEventListener?.('online', () => {
        publish({ health: 'recovering', writable: false });
        refreshAll({ force: true, reason: 'online' })
          .then((result) => rootObject.CFF_UI?.notify?.(
            result?.partial
              ? 'Server connection restored, but some league data is still stale.'
              : 'Server connection restored. League data refreshed.',
            result?.partial ? 'warning' : 'success'
          ))
          .catch(() => publish({ health: 'unavailable', writable: false }));
      });
      rootObject.addEventListener?.('storage', (event) => {
        if (event.key !== DATA_REVISION_KEY || !isServerSession(rootObject)) return;
        const revision = readJson(storage, DATA_REVISION_KEY, {}) || {};
        if (revision.leagueId && revision.leagueId !== activeLeagueId()) return;
        refreshAll({ reason: 'cross-tab' }).catch(() => {});
      });
      const refreshOnFocus = () => {
        const status = currentStatus();
        if (!isServerSession(rootObject) || status.health === 'healthy') return;
        if (now() - lastFocusRefreshAt < FOCUS_REFRESH_THROTTLE_MS) return;
        lastFocusRefreshAt = now();
        refreshAll({ reason: 'focus' }).catch(() => {});
      };
      rootObject.addEventListener?.('focus', refreshOnFocus);
      rootObject.document?.addEventListener?.('visibilitychange', () => {
        if (rootObject.document.visibilityState === 'visible') refreshOnFocus();
      });
    }

    function initializeSafetyGate() {
      if (!isServerSession(rootObject)) return;
      const status = currentStatus();
      if (!status.health || status.health === 'unknown') {
        publish({ health: online() ? 'unknown' : 'offline', writable: false });
      }
    }

    return {
      installApiWrapper,
      installSyncFunctions,
      installEvents,
      initializeSafetyGate,
      refreshLeagues,
      refreshActiveCollections,
      refreshDraft,
      refreshAll,
      controlsDisabled,
      recordCommittedMutation,
      hasRecentCommit,
      currentStatus,
      applyMutationResult: (path, result) => applyMutationResult(rootObject, path, result)
    };
  }

  const helpers = {
    SYNC_STATUS_KEY,
    CACHE_META_KEY,
    DATA_REVISION_KEY,
    MUTATION_METHODS,
    RECENT_COMMIT_TTL_MS,
    REFRESH_DEDUPE_MS,
    methodName,
    normalizePath,
    leagueIdFromPath,
    isServerSession,
    compactError,
    syncFailure,
    resultArray,
    applyMutationResult,
    createCoordinator
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  const coordinator = createCoordinator(root);
  root.CFFReliableSync = coordinator;
  let attempts = 0;
  function installWhenReady() {
    attempts += 1;
    coordinator.installApiWrapper();
    const ready = coordinator.installSyncFunctions();
    coordinator.installEvents();
    coordinator.initializeSafetyGate();
    if (!ready && attempts < 300) root.setTimeout(installWhenReady, 0);
    if (ready) {
      document.documentElement.dataset.cffSyncReady = 'true';
      document.documentElement.dataset.cffReliableSync = 'true';
    }
  }
  root.setTimeout(installWhenReady, 0);
})(typeof window !== 'undefined' ? window : globalThis);
