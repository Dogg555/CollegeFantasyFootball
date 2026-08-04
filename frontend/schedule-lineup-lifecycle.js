(() => {
  const OPERATION_PREFIX = 'cff:schedule-lineup:operation:';
  const CHANNEL_NAME = 'cff-schedule-lineup';
  const stateByLeague = new Map();
  let channel = null;

  function activeLeague() {
    return typeof window.getLeagueState === 'function' ? window.getLeagueState() : null;
  }

  function seasonValue(value) {
    return Number(value || new Date().getFullYear());
  }

  function operationStorageKey(leagueId, operation) {
    return `${OPERATION_PREFIX}${leagueId}:${operation}`;
  }

  function operationKey(leagueId, operation) {
    const key = operationStorageKey(leagueId, operation);
    let value = sessionStorage.getItem(key);
    if (!value) {
      value = `${operation}-${leagueId}-${Date.now()}-${Math.random().toString(36).slice(2)}`;
      sessionStorage.setItem(key, value);
    }
    return value;
  }

  function clearOperation(leagueId, operation) {
    sessionStorage.removeItem(operationStorageKey(leagueId, operation));
  }

  function acceptState(state) {
    if (!state?.leagueId) return state;
    const current = stateByLeague.get(state.leagueId);
    if (current && Number(state.version || 0) < Number(current.version || 0)) return current;
    stateByLeague.set(state.leagueId, state);
    if (Array.isArray(state.matchups) && typeof window.saveMatchups === 'function') {
      window.saveMatchups(state.matchups);
    }
    window.dispatchEvent(new CustomEvent('cff:schedule-lineup-state', { detail: state }));
    return state;
  }

  async function request(path, options = {}) {
    if (typeof window.apiRequest !== 'function') throw new Error('API client unavailable');
    return window.apiRequest(path, options);
  }

  async function fetchScheduleLineupState(season = new Date().getFullYear()) {
    const league = activeLeague();
    if (!league?.id) throw new Error('No server league selected');
    const state = await request(`/leagues/${encodeURIComponent(league.id)}/schedule/state?season=${encodeURIComponent(seasonValue(season))}`);
    return acceptState(state);
  }

  async function mutate(operation, path, body, expectedVersion) {
    const league = activeLeague();
    if (!league?.id) throw new Error('No server league selected');
    const key = operationKey(league.id, operation);
    try {
      const result = await request(path, {
        method: 'POST',
        headers: { 'Idempotency-Key': key },
        body: JSON.stringify({ ...body, expectedVersion })
      });
      clearOperation(league.id, operation);
      const accepted = acceptState(result);
      channel?.postMessage({ leagueId: league.id, version: accepted?.version || 0 });
      return accepted;
    } catch (error) {
      if (error?.status === 409 || error?.status === 428) {
        clearOperation(league.id, operation);
        try { await fetchScheduleLineupState(body?.season); } catch {}
      } else if (error?.status && error.status < 500) {
        clearOperation(league.id, operation);
      }
      throw error;
    }
  }

  async function generateSeasonScheduleReliable(weeks = 12, season = new Date().getFullYear()) {
    const league = activeLeague();
    const current = stateByLeague.get(league?.id) || await fetchScheduleLineupState(season);
    return mutate('generate', `/leagues/${encodeURIComponent(league.id)}/schedule/generate`,
      { weeks: Number(weeks), season: seasonValue(season) }, Number(current?.version || 0));
  }

  function weekState(state, week) {
    return (state?.lineupWeeks || []).find((item) => Number(item.week) === Number(week)) || null;
  }

  async function setLineupWeekLock(week, locked, options = {}) {
    const league = activeLeague();
    const season = seasonValue(options.season);
    const current = stateByLeague.get(league?.id) || await fetchScheduleLineupState(season);
    const selected = weekState(current, week);
    const action = locked ? 'lock' : 'unlock';
    const operation = `${action}:${season}:${week}`;
    return mutate(operation,
      `/leagues/${encodeURIComponent(league.id)}/lineups/week/${encodeURIComponent(week)}/${action}`,
      { season, lineupDeadline: options.lineupDeadline || '' }, Number(selected?.version || 0));
  }

  function lineupWeekLocked(week, season = new Date().getFullYear()) {
    const league = activeLeague();
    const state = stateByLeague.get(league?.id);
    const selected = weekState(state, week);
    if (!selected) return false;
    if (String(selected.status).toLowerCase() === 'final') return true;
    if (selected.locked || String(selected.status).toLowerCase() === 'locked') return true;
    return Boolean(selected.lineupDeadline && new Date() >= new Date(selected.lineupDeadline));
  }

  function installRecovery() {
    if ('BroadcastChannel' in window) {
      channel = new BroadcastChannel(CHANNEL_NAME);
      channel.onmessage = (event) => {
        const league = activeLeague();
        if (event.data?.leagueId === league?.id) fetchScheduleLineupState().catch(() => {});
      };
    }
    window.addEventListener('online', () => fetchScheduleLineupState().catch(() => {}));
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) fetchScheduleLineupState().catch(() => {});
    });
  }

  window.fetchScheduleLineupState = fetchScheduleLineupState;
  window.generateSeasonScheduleReliable = generateSeasonScheduleReliable;
  window.setLineupWeekLock = setLineupWeekLock;
  window.lineupWeekLocked = lineupWeekLocked;
  window.getScheduleLineupState = () => stateByLeague.get(activeLeague()?.id) || null;
  installRecovery();
})();
