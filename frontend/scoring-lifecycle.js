(function initScoringLifecycle(root) {
  'use strict';

  const OPERATION_STORAGE_KEY = 'cff_scoring_lifecycle_operations';
  const REVISION_STORAGE_KEY = 'cff_scoring_lifecycle_revision';
  const MAX_OPERATION_AGE_MS = 15 * 60 * 1000;
  const states = new Map();
  let latestState = null;
  let latestStandings = [];
  let installAttempts = 0;

  function normalizeVersion(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
  }

  function stateKey(leagueId, season, week) {
    return `${String(leagueId || '')}:${Number(season) || 0}:${Number(week) || 1}`;
  }

  function stateVersion(state) {
    return normalizeVersion(state?.weekVersion ?? state?.version ?? 0);
  }

  function globalVersion(state) {
    return normalizeVersion(state?.globalVersion ?? 0);
  }

  function standingsVersion(state) {
    return normalizeVersion(state?.standingsVersion ?? 0);
  }

  function shouldApplyState(current, incoming) {
    if (!incoming || typeof incoming !== 'object') return false;
    if (!current) return true;
    if (globalVersion(incoming) !== globalVersion(current)) {
      return globalVersion(incoming) > globalVersion(current);
    }
    if (stateVersion(incoming) !== stateVersion(current)) {
      return stateVersion(incoming) > stateVersion(current);
    }
    return standingsVersion(incoming) >= standingsVersion(current);
  }

  function createOperationId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID();
    const stamp = typeof now === 'function' ? now() : now;
    const entropy = typeof random === 'function' ? random() : random;
    return `scoring-${Math.max(0, Number(stamp) || 0).toString(36)}-${Math.floor((Number(entropy) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
  }

  function readOperations(storage = root.sessionStorage) {
    try {
      return JSON.parse(storage?.getItem?.(OPERATION_STORAGE_KEY) || '{}') || {};
    } catch {
      return {};
    }
  }

  function writeOperations(operations, storage = root.sessionStorage) {
    try {
      storage?.setItem?.(OPERATION_STORAGE_KEY, JSON.stringify(operations));
    } catch {
      // Session persistence is best-effort.
    }
  }

  function operationFor(action, leagueId, season, week, fingerprint = '', storage = root.sessionStorage, createId = createOperationId) {
    const operations = readOperations(storage);
    const key = `${String(leagueId || '')}:${String(action || '')}:${Number(season) || 0}:${Number(week) || 1}`;
    const existing = operations[key];
    const age = Date.now() - Number(existing?.createdAt || 0);
    if (existing?.operationKey && existing.fingerprint === fingerprint && age >= 0 && age < MAX_OPERATION_AGE_MS) {
      return existing;
    }
    const operation = {
      action,
      leagueId: String(leagueId || ''),
      season: Number(season) || 0,
      week: Number(week) || 1,
      fingerprint,
      operationKey: createId(),
      createdAt: Date.now()
    };
    operations[key] = operation;
    writeOperations(operations, storage);
    return operation;
  }

  function clearOperation(action, leagueId, season, week, operationKey = '', storage = root.sessionStorage) {
    const operations = readOperations(storage);
    const key = `${String(leagueId || '')}:${String(action || '')}:${Number(season) || 0}:${Number(week) || 1}`;
    if (!operations[key]) return;
    if (operationKey && operations[key].operationKey !== operationKey) return;
    delete operations[key];
    writeOperations(operations, storage);
  }

  function uncertainFailure(error) {
    const status = Number(error?.status || 0);
    return Boolean(error?.timedOut || error?.unavailable || error?.retryable || !status || status >= 500);
  }

  function scoringErrorMessage(error, fallback = 'The scoring action could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      scoring_state_conflict: 'The week changed on the server. The latest scoring state has been loaded.',
      week_finalized: 'This week is final and its scores can no longer be changed.',
      week_not_scored: 'Score the week before finalizing it.',
      invalid_lineup: 'One or more active managers have an invalid starting lineup.',
      commissioner_required: 'Only the league commissioner can score or finalize a week.',
      matchups_unavailable: 'At least two active managers are required to score this week.',
      idempotency_key_conflict: 'This scoring request key was already used for another action.'
    };
    if (messages[code]) return messages[code];
    if (uncertainFailure(error)) {
      return 'The server may have accepted this scoring action. Retry safely; the same operation will not run twice.';
    }
    return error?.userMessage
      || root.mutationErrorMessage?.(error, fallback)
      || error?.data?.error
      || error?.message
      || fallback;
  }

  function currentLeague() {
    return root.getLeagueState?.() || null;
  }

  function currentSeason() {
    return Number(latestState?.season) || new Date().getFullYear();
  }

  function currentWeek() {
    return Number(latestState?.week) || 1;
  }

  function publishState(state) {
    const payload = {
      leagueId: String(state?.leagueId || currentLeague()?.id || ''),
      season: Number(state?.season) || 0,
      week: Number(state?.week) || 1,
      globalVersion: globalVersion(state),
      weekVersion: stateVersion(state),
      standingsVersion: standingsVersion(state),
      at: new Date().toISOString()
    };
    try {
      root.localStorage?.setItem?.(REVISION_STORAGE_KEY, JSON.stringify(payload));
      root.dispatchEvent?.(new CustomEvent('cff:scoring-lifecycle', { detail: payload }));
    } catch {
      // Storage and CustomEvent may be unavailable in focused tests.
    }
  }

  function applyState(state) {
    if (!state || typeof state !== 'object') return latestState;
    const leagueId = String(state.leagueId || currentLeague()?.id || '');
    const season = Number(state.season) || currentSeason();
    const week = Number(state.week) || currentWeek();
    const key = stateKey(leagueId, season, week);
    const current = states.get(key) || null;
    if (!shouldApplyState(current, state)) return current;
    states.set(key, state);
    latestState = state;
    root.__cffScoringLifecycleVersion = stateVersion(state);
    root.__cffScoringGlobalVersion = globalVersion(state);
    root.__cffStandingsVersion = standingsVersion(state);
    if (Array.isArray(state.matchups)) {
      const existing = root.getMatchups?.() || [];
      const others = existing.filter((matchup) => Number(matchup.week || 1) !== week
        || (Number(matchup.season || season) !== season));
      root.saveMatchups?.([...others, ...state.matchups]);
    }
    if (Array.isArray(state.standings)) latestStandings = state.standings;
    root.writeApiCacheMeta?.('league', leagueId);
    publishState(state);
    return state;
  }

  async function syncState(season = currentSeason(), week = currentWeek()) {
    const league = currentLeague();
    if (!root.getAuthState?.()?.token || !league?.id || root.isLocalDemoSession?.()) return null;
    const state = await root.apiRequest(
      `/leagues/${encodeURIComponent(league.id)}/scoring/state?season=${encodeURIComponent(season)}&week=${encodeURIComponent(week)}`
    );
    applyState(state);
    return state;
  }

  async function requestMutation(action, season, week) {
    const league = currentLeague();
    if (!league?.id) throw new Error('No server league selected');
    const key = stateKey(league.id, season, week);
    if (!states.has(key)) await syncState(season, week);
    const current = states.get(key) || latestState || {};
    const operation = operationFor(action, league.id, season, week, `${action}:${stateVersion(current)}`);
    const request = () => root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/scoring/transactions`, {
      method: 'POST',
      headers: { 'Idempotency-Key': operation.operationKey },
      body: JSON.stringify({
        action,
        season,
        week,
        expectedVersion: stateVersion(current)
      }),
      cffSkipMutationRefresh: true
    });

    try {
      let state;
      try {
        state = await request();
      } catch (firstError) {
        if (!uncertainFailure(firstError)) throw firstError;
        state = await request();
      }
      clearOperation(action, league.id, season, week, operation.operationKey);
      applyState(state);
      try {
        await root.syncActiveLeagueCollectionsFromApi?.();
      } catch {
        // The confirmed scoring state remains authoritative.
      }
      root.renderLeague?.();
      return state;
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, league.id, season, week, operation.operationKey);
      const conflictState = error?.data?.state;
      if (conflictState && typeof conflictState === 'object') applyState(conflictState);
      else if (error?.status === 409) {
        try {
          await syncState(season, week);
        } catch {
          // Keep the last confirmed state if recovery is unavailable.
        }
      }
      error.userMessage = scoringErrorMessage(error);
      throw error;
    }
  }

  function install() {
    installAttempts += 1;
    const required = ['apiRequest', 'scoreWeekApi', 'finalizeWeekApi', 'standingsFromMatchups'];
    if (!required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 400) root.setTimeout?.(install, 0);
      return;
    }
    if (root.scoreWeekApi.__cffScoringLifecycle) return;

    const originals = Object.fromEntries(required.slice(1).map((name) => [name, root[name]]));

    root.scoreWeekApi = async function resilientScoreWeek(week = 1, season = new Date().getFullYear()) {
      if (root.isLocalDemoSession?.()) return originals.scoreWeekApi.call(this, week, season);
      return requestMutation('score', Number(season) || new Date().getFullYear(), Number(week) || 1);
    };

    root.finalizeWeekApi = async function resilientFinalizeWeek(week = 1, season = currentSeason()) {
      if (root.isLocalDemoSession?.()) return originals.finalizeWeekApi.call(this, week, season);
      return requestMutation('finalize', Number(season) || new Date().getFullYear(), Number(week) || 1);
    };

    root.standingsFromMatchups = function authoritativeStandings(league, matchups) {
      const activeLeagueId = String(league?.id || currentLeague()?.id || '');
      if (latestState && String(latestState.leagueId || '') === activeLeagueId && Array.isArray(latestStandings)) {
        return latestStandings;
      }
      return originals.standingsFromMatchups.call(this, league, matchups);
    };

    root.scoreWeekApi.__cffScoringLifecycle = true;
    root.finalizeWeekApi.__cffScoringLifecycle = true;
    root.standingsFromMatchups.__cffScoringLifecycle = true;

    root.addEventListener?.('online', () => {
      void syncState().then(() => root.renderLeague?.()).catch(() => {});
    });
    root.addEventListener?.('storage', (event) => {
      if (event.key !== REVISION_STORAGE_KEY) return;
      void syncState().then(() => root.renderLeague?.()).catch(() => {});
    });
    root.document?.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') {
        void syncState().then(() => root.renderLeague?.()).catch(() => {});
      }
    });

    root.CFFScoringLifecycle = Object.freeze({
      installed: true,
      sync: syncState,
      latest: () => latestState,
      standings: () => latestStandings,
      currentVersion: () => stateVersion(latestState),
      globalVersion: () => globalVersion(latestState),
      standingsVersion: () => standingsVersion(latestState),
      errorMessage: scoringErrorMessage
    });
    root.document?.documentElement?.setAttribute?.('data-cff-scoring-lifecycle', 'true');
    void syncState().then(() => root.renderLeague?.()).catch(() => {});
  }

  const helpers = {
    OPERATION_STORAGE_KEY,
    REVISION_STORAGE_KEY,
    normalizeVersion,
    stateVersion,
    globalVersion,
    standingsVersion,
    shouldApplyState,
    createOperationId,
    operationFor,
    clearOperation,
    uncertainFailure,
    scoringErrorMessage
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  install();
})(typeof window !== 'undefined' ? window : globalThis);
