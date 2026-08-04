(() => {
  const CACHE_KEY = 'cff_schedule_lineup_state_v1';
  const OPERATION_PREFIX = 'cff_schedule_lineup_operation_v1';
  const BROADCAST_NAME = 'cff-schedule-lineup';
  let currentState = null;
  let installed = false;
  let channel = null;

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
      // Storage is an optimization; the server remains authoritative.
    }
  }

  function league() {
    return typeof window.getLeagueState === 'function' ? window.getLeagueState() : null;
  }

  function auth() {
    return typeof window.getAuthState === 'function' ? window.getAuthState() : null;
  }

  function localDemo() {
    return typeof window.isLocalDemoSession === 'function' && window.isLocalDemoSession();
  }

  function seasonValue(value = new Date().getFullYear()) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed > 0 ? Math.trunc(parsed) : new Date().getFullYear();
  }

  function weekValue(value = 1) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed > 0 ? Math.trunc(parsed) : 1;
  }

  function cacheIdentity(state = currentState) {
    return `${state?.leagueId || league()?.id || ''}:${state?.season || new Date().getFullYear()}:${state?.week || 1}`;
  }

  function operationStorageKey(action, state, extra = '') {
    return `${OPERATION_PREFIX}:${action}:${cacheIdentity(state)}:${extra}`;
  }

  function randomKey(action) {
    const random = globalThis.crypto?.randomUUID?.()
      || `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
    return `schedule-${action}-${random}`;
  }

  function operationKey(action, state, extra = '') {
    const key = operationStorageKey(action, state, extra);
    const existing = window.sessionStorage?.getItem?.(key);
    if (existing) return existing;
    const created = randomKey(action);
    window.sessionStorage?.setItem?.(key, created);
    return created;
  }

  function clearOperationKey(action, state, extra = '') {
    window.sessionStorage?.removeItem?.(operationStorageKey(action, state, extra));
  }

  function applyState(next, { broadcast = true } = {}) {
    if (!next || typeof next !== 'object') return currentState;
    const nextVersion = Number(next.scheduleVersion ?? next.version ?? 0);
    const currentVersion = Number(currentState?.scheduleVersion ?? currentState?.version ?? -1);
    const sameScope = currentState
      && currentState.leagueId === next.leagueId
      && Number(currentState.season) === Number(next.season);
    if (sameScope && nextVersion < currentVersion) return currentState;

    currentState = next;
    writeJson(window.localStorage, CACHE_KEY, next);
    if (Array.isArray(next.schedule) && typeof window.saveMatchups === 'function') {
      window.saveMatchups(next.schedule);
    }
    window.dispatchEvent?.(new CustomEvent('cff:schedule-lineup-state', { detail: next }));
    renderStatus(next);
    if (broadcast) channel?.postMessage?.({ type: 'state', state: next });
    return currentState;
  }

  function cachedState() {
    if (currentState) return currentState;
    const cached = readJson(window.localStorage, CACHE_KEY, null);
    if (cached && cached.leagueId === league()?.id) currentState = cached;
    return currentState;
  }

  async function requestState(season = new Date().getFullYear(), week = 1) {
    const selected = league();
    if (!selected?.id || !auth()?.token || localDemo()) return cachedState();
    const state = await window.apiRequest(
      `/leagues/${encodeURIComponent(selected.id)}/schedule/state?season=${encodeURIComponent(seasonValue(season))}&week=${encodeURIComponent(weekValue(week))}`
    );
    return applyState(state);
  }

  function retryable(error) {
    return !error?.status || Number(error.status) >= 500 || error?.status === 429;
  }

  async function mutate(action, payload = {}, options = {}) {
    const selected = league();
    if (!selected?.id) throw new Error('No server league selected');
    let state = cachedState();
    const season = seasonValue(payload.season ?? state?.season);
    const week = weekValue(payload.week ?? state?.week);
    if (!state || state.leagueId !== selected.id || Number(state.season) !== season) {
      state = await requestState(season, week);
    }
    const extra = options.extra || (payload.managerEmail ? String(payload.managerEmail).toLowerCase() : payload.all ? 'all' : 'self');
    const key = operationKey(action, { ...state, season, week }, extra);
    try {
      const result = await window.apiRequest(
        `/leagues/${encodeURIComponent(selected.id)}/schedule/transactions`,
        {
          method: 'POST',
          headers: { 'Idempotency-Key': key },
          body: JSON.stringify({
            ...payload,
            action,
            season,
            week,
            expectedVersion: Number(state?.scheduleVersion ?? state?.version ?? 0)
          })
        }
      );
      clearOperationKey(action, { ...state, season, week }, extra);
      return applyState(result);
    } catch (error) {
      const authoritative = error?.data?.currentState;
      if (authoritative) applyState(authoritative);
      if (!retryable(error)) clearOperationKey(action, { ...state, season, week }, extra);
      throw error;
    }
  }

  async function generateSchedule(weeks = 12, season = new Date().getFullYear()) {
    if (localDemo()) return null;
    const state = await mutate('generate', {
      season: seasonValue(season),
      week: 1,
      weeks: Math.max(1, Math.min(15, Number(weeks) || 12))
    }, { extra: String(weeks) });
    return state?.schedule || [];
  }

  async function setDeadline(week, lineupDeadline, season = new Date().getFullYear()) {
    return mutate('set_deadline', {
      season: seasonValue(season),
      week: weekValue(week),
      lineupDeadline: lineupDeadline || ''
    }, { extra: String(weekValue(week)) });
  }

  async function lockLineup(week, options = {}) {
    return mutate('lock', {
      season: seasonValue(options.season),
      week: weekValue(week),
      managerEmail: options.managerEmail || undefined,
      all: options.all === true
    }, { extra: options.all ? 'all' : options.managerEmail || 'self' });
  }

  async function unlockLineup(week, options = {}) {
    return mutate('unlock', {
      season: seasonValue(options.season),
      week: weekValue(week),
      managerEmail: options.managerEmail || undefined,
      all: options.all === true
    }, { extra: options.all ? 'all' : options.managerEmail || 'self' });
  }

  function lockedForCurrentManager(state = cachedState()) {
    return Boolean(state?.lineupLocked || ['locked', 'finalized'].includes(String(state?.myLineup?.status || '').toLowerCase()));
  }

  function renderStatus(state = cachedState()) {
    const target = document.querySelector?.('[data-cff-lineup-lock-status]');
    if (!target || !state) return;
    const locked = lockedForCurrentManager(state);
    const deadline = state.lineupDeadline ? new Date(state.lineupDeadline).toLocaleString() : 'No deadline set';
    target.textContent = locked
      ? `Week ${state.week} lineup locked (${state.lockReason || state.myLineup?.lockReason || 'manual'}).`
      : `Week ${state.week} lineup open. ${deadline}.`;
    target.dataset.state = locked ? 'locked' : 'open';
  }

  function installWrappers() {
    if (installed || typeof window.apiRequest !== 'function' || typeof window.getLeagueState !== 'function') return false;
    installed = true;

    const originalGenerate = window.generateSeasonScheduleApi;
    if (typeof originalGenerate === 'function') {
      window.generateSeasonScheduleApi = async (weeks = 12, season = new Date().getFullYear()) => {
        if (!auth()?.token || localDemo()) return originalGenerate(weeks);
        return generateSchedule(weeks, season);
      };
    }

    const originalSlot = window.updateRosterSlotApi;
    if (typeof originalSlot === 'function') {
      window.updateRosterSlotApi = async (playerId, slot) => {
        if (auth()?.token && !localDemo() && lockedForCurrentManager()) {
          const error = new Error('The active weekly lineup is locked.');
          error.status = 409;
          error.data = { code: 'lineup_locked', error: error.message };
          throw error;
        }
        return originalSlot(playerId, slot);
      };
    }

    const originalScore = window.scoreWeekApi;
    if (typeof originalScore === 'function') {
      window.scoreWeekApi = async (...args) => {
        const result = await originalScore(...args);
        if (auth()?.token && !localDemo()) {
          await requestState(args[1] || new Date().getFullYear(), args[0] || 1).catch(() => null);
        }
        return result;
      };
    }

    const originalFinalize = window.finalizeWeekApi;
    if (typeof originalFinalize === 'function') {
      window.finalizeWeekApi = async (...args) => {
        const result = await originalFinalize(...args);
        if (auth()?.token && !localDemo()) {
          await requestState(new Date().getFullYear(), args[0] || 1).catch(() => null);
        }
        return result;
      };
    }

    window.getScheduleLineupStateApi = requestState;
    window.generateDeterministicScheduleApi = generateSchedule;
    window.setLineupDeadlineApi = setDeadline;
    window.lockWeeklyLineupApi = lockLineup;
    window.unlockWeeklyLineupApi = unlockLineup;
    window.lineupLockedByServer = lockedForCurrentManager;
    return true;
  }

  function refreshVisibleState() {
    if (document.visibilityState === 'hidden' || !auth()?.token || localDemo() || !league()?.id) return;
    const state = cachedState();
    requestState(state?.season || new Date().getFullYear(), state?.week || 1).catch(() => null);
  }

  try {
    channel = typeof BroadcastChannel === 'function' ? new BroadcastChannel(BROADCAST_NAME) : null;
    if (channel) {
      channel.onmessage = (event) => {
        if (event?.data?.type === 'state') applyState(event.data.state, { broadcast: false });
      };
    }
  } catch {
    channel = null;
  }

  window.addEventListener?.('storage', (event) => {
    if (event.key !== CACHE_KEY || !event.newValue) return;
    try { applyState(JSON.parse(event.newValue), { broadcast: false }); } catch {}
  });
  window.addEventListener?.('online', refreshVisibleState);
  document.addEventListener?.('visibilitychange', refreshVisibleState);

  let installTimer = null;
  installTimer = setInterval(() => {
    if (installWrappers() && installTimer !== null) {
      clearInterval(installTimer);
      installTimer = null;
      const state = cachedState();
      if (state) renderStatus(state);
    }
  }, 25);
  setTimeout(() => {
    if (installTimer !== null) clearInterval(installTimer);
    installTimer = null;
  }, 5000);
  installWrappers();

  window.CFFScheduleLineupLifecycle = {
    applyState,
    cachedState,
    operationKey,
    clearOperationKey,
    mutate,
    requestState,
    lockedForCurrentManager,
    installWrappers
  };
})();
