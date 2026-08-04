(function initScheduleLineupLifecycle(root) {
  'use strict';

  const OPERATION_STORAGE_KEY = 'cff_schedule_lifecycle_operations';
  const REVISION_STORAGE_KEY = 'cff_schedule_lifecycle_revision';
  const MAX_OPERATION_AGE_MS = 15 * 60 * 1000;
  let latestState = null;
  let installAttempts = 0;

  function normalizeVersion(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
  }

  function stateVersion(state) {
    return normalizeVersion(state?.scheduleVersion ?? state?.version ?? 0);
  }

  function shouldApplyState(current, incoming) {
    if (!incoming || typeof incoming !== 'object') return false;
    if (!current) return true;
    return stateVersion(incoming) >= stateVersion(current);
  }

  function createOperationId(cryptoObject = root.crypto, now = Date.now, random = Math.random) {
    if (cryptoObject && typeof cryptoObject.randomUUID === 'function') return cryptoObject.randomUUID();
    const stamp = typeof now === 'function' ? now() : now;
    const entropy = typeof random === 'function' ? random() : random;
    return `schedule-${Math.max(0, Number(stamp) || 0).toString(36)}-${Math.floor((Number(entropy) || 0) * Number.MAX_SAFE_INTEGER).toString(36)}`;
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
    const key = `${String(leagueId || '')}:${String(action || '')}:${Number(season) || 0}:${Number(week) || 0}`;
    const existing = operations[key];
    const age = Date.now() - Number(existing?.createdAt || 0);
    if (existing?.operationKey && existing.fingerprint === fingerprint && age >= 0 && age < MAX_OPERATION_AGE_MS) {
      return existing;
    }
    const operation = {
      action,
      leagueId: String(leagueId || ''),
      season: Number(season) || 0,
      week: Number(week) || 0,
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
    const key = `${String(leagueId || '')}:${String(action || '')}:${Number(season) || 0}:${Number(week) || 0}`;
    if (!operations[key]) return;
    if (operationKey && operations[key].operationKey !== operationKey) return;
    delete operations[key];
    writeOperations(operations, storage);
  }

  function uncertainFailure(error) {
    const status = Number(error?.status || 0);
    return Boolean(error?.timedOut || error?.unavailable || error?.retryable || !status || status >= 500);
  }

  function scheduleErrorMessage(error, fallback = 'The schedule action could not be completed.') {
    const code = String(error?.data?.code || error?.code || '');
    const messages = {
      schedule_state_conflict: 'The schedule changed on the server. The latest version has been loaded.',
      schedule_locked: 'The schedule cannot change after a lineup is locked or scoring begins.',
      lineup_locked: 'The current week lineup is locked.',
      lineup_finalized: 'This week is final and cannot be unlocked.',
      lineup_unlock_forbidden: 'A lineup cannot be unlocked after scoring begins.',
      commissioner_required: 'Only the league commissioner can change the schedule or lineup controls.',
      insufficient_active_managers: 'At least two active managers are required.',
      idempotency_key_conflict: 'This request key was already used for another schedule action.'
    };
    if (messages[code]) return messages[code];
    if (uncertainFailure(error)) {
      return 'The server may have accepted this action. Retry safely; the same operation will not run twice.';
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
    return Number(latestState?.currentWeek) || 1;
  }

  function controlForWeek(week = currentWeek(), state = latestState) {
    return (state?.weekControls || []).find((control) => Number(control.week) === Number(week)) || null;
  }

  function lockedForWeek(week = currentWeek(), state = latestState) {
    const status = String(controlForWeek(week, state)?.status || 'open').toLowerCase();
    return status === 'locked' || status === 'finalized';
  }

  function publishState(state) {
    const payload = {
      leagueId: String(state?.leagueId || currentLeague()?.id || ''),
      season: Number(state?.season) || 0,
      currentWeek: Number(state?.currentWeek) || 1,
      version: stateVersion(state),
      at: new Date().toISOString()
    };
    try {
      root.localStorage?.setItem?.(REVISION_STORAGE_KEY, JSON.stringify(payload));
      root.dispatchEvent?.(new CustomEvent('cff:schedule-lifecycle', { detail: payload }));
    } catch {
      // Storage and CustomEvent may be unavailable in focused tests.
    }
  }

  function applyState(state) {
    if (!shouldApplyState(latestState, state)) return latestState;
    latestState = state;
    root.__cffScheduleVersion = stateVersion(state);
    root.__cffCurrentWeek = currentWeek();
    root.__cffLineupLocked = lockedForWeek();
    if (Array.isArray(state.matchups)) root.saveMatchups?.(state.matchups);
    root.writeApiCacheMeta?.('league', state.leagueId || currentLeague()?.id || '');
    publishState(state);
    return state;
  }

  async function syncState(season = currentSeason()) {
    const league = currentLeague();
    if (!root.getAuthState?.()?.token || !league?.id || root.isLocalDemoSession?.()) return null;
    const state = await root.apiRequest(
      `/leagues/${encodeURIComponent(league.id)}/schedule/state?season=${encodeURIComponent(season)}`
    );
    applyState(state);
    return state;
  }

  async function requestMutation(action, options = {}) {
    const league = currentLeague();
    if (!league?.id) throw new Error('No server league selected');
    if (!latestState) await syncState(options.season || currentSeason());
    const season = Number(options.season || latestState?.season || new Date().getFullYear());
    const week = Number(options.week || latestState?.currentWeek || 1);
    const fingerprint = JSON.stringify({ action, season, week, version: stateVersion(latestState), ...options });
    const operation = operationFor(action, league.id, season, week, fingerprint);
    const request = () => root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/schedule/transactions`, {
      method: 'POST',
      headers: { 'Idempotency-Key': operation.operationKey },
      body: JSON.stringify({
        action,
        season,
        week,
        expectedVersion: stateVersion(latestState),
        ...options
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
      try {
        await root.syncActiveLeagueCollectionsFromApi?.();
      } catch {
        // The confirmed schedule response remains authoritative.
      }
      applyState(state);
      root.renderLeague?.();
      return state;
    } catch (error) {
      if (!uncertainFailure(error)) clearOperation(action, league.id, season, week, operation.operationKey);
      const conflict = error?.data?.currentState;
      if (conflict && typeof conflict === 'object') applyState(conflict);
      else if (error?.status === 409) {
        try { await syncState(season); } catch { /* keep last confirmed state */ }
      }
      error.userMessage = scheduleErrorMessage(error);
      throw error;
    }
  }

  function install() {
    installAttempts += 1;
    const required = ['apiRequest', 'generateSeasonScheduleApi', 'updateRosterSlotApi', 'lineupLocked'];
    if (!required.every((name) => typeof root[name] === 'function')) {
      if (installAttempts < 400) root.setTimeout?.(install, 0);
      return;
    }
    if (root.generateSeasonScheduleApi.__cffScheduleLifecycle) return;

    const originalGenerate = root.generateSeasonScheduleApi;
    const originalRosterSlot = root.updateRosterSlotApi;
    const originalLineupLocked = root.lineupLocked;

    root.generateSeasonScheduleApi = async function resilientGenerateSchedule(weeks = 12, season = new Date().getFullYear()) {
      if (root.isLocalDemoSession?.()) return originalGenerate.call(this, weeks);
      const state = await requestMutation('generate', { weeks: Number(weeks) || 12, season: Number(season) || new Date().getFullYear(), week: 1 });
      return state?.matchups || [];
    };

    root.updateRosterSlotApi = async function guardedRosterSlot(playerId, slot) {
      if (!root.isLocalDemoSession?.() && lockedForWeek()) {
        const error = new Error('The current week lineup is locked.');
        error.status = 409;
        error.code = 'lineup_locked';
        error.userMessage = scheduleErrorMessage(error);
        throw error;
      }
      const result = await originalRosterSlot.call(this, playerId, slot);
      if (!root.isLocalDemoSession?.()) {
        try { await syncState(); } catch { /* roster response remains authoritative */ }
      }
      return result;
    };

    root.lineupLocked = function authoritativeLineupLocked() {
      if (latestState) return lockedForWeek();
      return originalLineupLocked.call(this);
    };

    root.lockLineupWeekApi = (week = currentWeek(), season = currentSeason()) => requestMutation('lock', { week, season });
    root.unlockLineupWeekApi = (week = currentWeek(), season = currentSeason()) => requestMutation('unlock', { week, season });
    root.setLineupDeadlineApi = (week, lineupDeadline, season = currentSeason()) => requestMutation('set_deadline', { week, lineupDeadline, season });
    root.advanceScheduleWeekApi = (currentWeekValue, season = currentSeason()) => requestMutation('advance_week', {
      week: Number(currentWeekValue) || currentWeek(),
      currentWeek: Number(currentWeekValue) || currentWeek(),
      season
    });

    root.generateSeasonScheduleApi.__cffScheduleLifecycle = true;
    root.updateRosterSlotApi.__cffScheduleLifecycle = true;
    root.lineupLocked.__cffScheduleLifecycle = true;

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

    root.CFFScheduleLifecycle = Object.freeze({
      installed: true,
      sync: syncState,
      latest: () => latestState,
      currentVersion: () => stateVersion(latestState),
      currentWeek,
      controlForWeek,
      lockedForWeek,
      errorMessage: scheduleErrorMessage
    });
    root.document?.documentElement?.setAttribute?.('data-cff-schedule-lifecycle', 'true');
    void syncState().then(() => root.renderLeague?.()).catch(() => {});
  }

  const helpers = {
    OPERATION_STORAGE_KEY,
    REVISION_STORAGE_KEY,
    normalizeVersion,
    stateVersion,
    shouldApplyState,
    createOperationId,
    operationFor,
    clearOperation,
    uncertainFailure,
    scheduleErrorMessage,
    controlForWeek,
    lockedForWeek
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  install();
})(typeof window !== 'undefined' ? window : globalThis);
