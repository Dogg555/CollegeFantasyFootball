(function initLineupManagement(root) {
  'use strict';

  const STARTER_ORDER = Object.freeze(['qb', 'rb', 'wr', 'te', 'flex', 'k', 'def']);
  const LOCK_CACHE_MS = 15000;
  let lockState = null;
  let lockStateAt = 0;
  let lockRequest = null;
  let stagedRoster = null;
  let stagedLeagueId = '';
  let saving = false;
  let installAttempts = 0;

  function normalizedRules(league = root.getLeagueState?.()) {
    return {
      qb: 1, rb: 2, wr: 2, te: 1, flex: 2, k: 0, def: 0, bench: 6,
      ...(league?.rosterRules || {})
    };
  }

  function starterSlots(rules = normalizedRules()) {
    return STARTER_ORDER.flatMap((slot) => {
      const count = Math.max(0, Number(rules?.[slot] || 0));
      return Array.from({ length: count }, (_, index) => ({
        slot,
        index,
        key: `${slot}-${index + 1}`,
        label: count > 1 ? `${slot.toUpperCase()} ${index + 1}` : slot.toUpperCase()
      }));
    });
  }

  function cloneRoster(roster = []) {
    return (Array.isArray(roster) ? roster : []).map((player) => ({ ...player }));
  }

  function assignmentsFromRoster(roster = []) {
    return cloneRoster(roster)
      .map((player) => ({
        playerId: String(player?.id || player?.playerId || ''),
        slot: String(player?.rosterSlot || 'bench').toLowerCase()
      }))
      .sort((left, right) => left.playerId.localeCompare(right.playerId));
  }

  function rosterSignature(roster = []) {
    return assignmentsFromRoster(roster)
      .map((assignment) => `${assignment.playerId}:${assignment.slot}`)
      .join('|');
  }

  function currentWeekContext() {
    const leagueId = String(root.getLeagueState?.()?.id || '');
    if (lockState
        && String(lockState.leagueId || '') === leagueId
        && Number(lockState.season) > 0
        && Number(lockState.week) > 0) {
      return { season: Number(lockState.season), week: Number(lockState.week) };
    }
    const schedule = root.CFFScheduleLineupLifecycle?.cachedState?.() || {};
    const scoreboardWeek = Number(root.document?.getElementById?.('scoreboard-week')?.value || 0);
    return {
      season: Math.max(1, Number(schedule.season || new Date().getFullYear())),
      week: Math.max(1, Number(schedule.week || scoreboardWeek || 1))
    };
  }

  function lockMap(state = lockState) {
    return new Map((Array.isArray(state?.players) ? state.players : [])
      .map((entry) => [String(entry.playerId || ''), entry]));
  }

  function playerLocked(player, state = lockState, context = currentWeekContext()) {
    if (state?.weekLocked
        && Number(state.season) === Number(context.season)
        && Number(state.week) === Number(context.week)) return true;
    const serverLock = lockMap(state).get(String(player?.id || player?.playerId || ''));
    if (serverLock
        && Number(serverLock.season) === Number(context.season)
        && Number(serverLock.week) === Number(context.week)) return serverLock.locked === true;
    return player?.locked === true || player?.gameStarted === true;
  }

  function groupRoster(roster = [], rules = normalizedRules()) {
    const startersBySlot = new Map();
    const bench = [];
    roster.forEach((player) => {
      const slot = String(player?.rosterSlot || 'bench').toLowerCase();
      if (slot === 'bench' || !STARTER_ORDER.includes(slot) || Number(rules?.[slot] || 0) <= 0) {
        bench.push(player);
        return;
      }
      if (!startersBySlot.has(slot)) startersBySlot.set(slot, []);
      startersBySlot.get(slot).push(player);
    });
    return { startersBySlot, bench };
  }

  function positionEligible(player, slot) {
    const position = String(player?.position || '').toLowerCase();
    const requested = String(slot || '').toLowerCase();
    if (requested === 'bench') return true;
    if (requested === 'flex') return ['rb', 'wr', 'te'].includes(position);
    return requested === position;
  }

  function slotOccupancy(roster = [], ignoredPlayerId = '') {
    return roster.reduce((counts, player) => {
      if (String(player?.id || '') === String(ignoredPlayerId || '')) return counts;
      const slot = String(player?.rosterSlot || 'bench').toLowerCase();
      counts[slot] = (counts[slot] || 0) + 1;
      return counts;
    }, {});
  }

  function legalDestinations(player, roster = [], rules = normalizedRules()) {
    const current = String(player?.rosterSlot || 'bench').toLowerCase();
    const occupied = slotOccupancy(roster, player?.id);
    return [...STARTER_ORDER, 'bench'].filter((slot) => {
      if (slot === current || Number(rules?.[slot] || 0) <= 0) return false;
      if (!positionEligible(player, slot)) return false;
      if (slot === 'bench') return true;
      return Number(occupied[slot] || 0) < Number(rules[slot] || 0);
    });
  }

  function lineupErrorsAllowEmpty(roster = root.getRoster?.() || [], league = root.getLeagueState?.()) {
    const rules = normalizedRules(league);
    const counts = slotOccupancy(roster);
    const errors = [];
    roster.forEach((player) => {
      const slot = String(player?.rosterSlot || 'bench').toLowerCase();
      if (slot !== 'bench' && !positionEligible(player, slot)) {
        errors.push({ slot, playerId: String(player?.id || ''), message: `${player?.name || 'Player'} is not eligible for ${slot.toUpperCase()}` });
      }
    });
    STARTER_ORDER.forEach((slot) => {
      const limit = Number(rules?.[slot] || 0);
      const filled = Number(counts[slot] || 0);
      if (filled > limit) errors.push({ slot, message: `Too many ${slot.toUpperCase()} starters` });
    });
    return errors;
  }

  function lineupSaveErrors(roster = root.getRoster?.() || [], league = root.getLeagueState?.()) {
    const errors = lineupErrorsAllowEmpty(roster, league);
    const rules = normalizedRules(league);
    const benchCount = Number(slotOccupancy(roster).bench || 0);
    if (benchCount > Number(rules.bench || 0)) {
      errors.push({ slot: 'bench', message: `Too many bench players (${benchCount}/${Number(rules.bench || 0)})` });
    }
    const ids = new Set();
    roster.forEach((player) => {
      const id = String(player?.id || player?.playerId || '');
      if (!id || ids.has(id)) errors.push({ playerId: id, message: 'Every rostered player must appear exactly once.' });
      ids.add(id);
    });
    return errors;
  }

  function stageMoveInRoster(roster, playerId, slot) {
    const working = cloneRoster(roster);
    const player = working.find((item) => String(item?.id || item?.playerId || '') === String(playerId || ''));
    if (!player) return null;
    player.rosterSlot = String(slot || '').toLowerCase();
    return working;
  }

  function emptyStarterCount(roster = [], rules = normalizedRules()) {
    const counts = slotOccupancy(roster);
    return STARTER_ORDER.reduce((total, slot) => (
      total + Math.max(0, Number(rules?.[slot] || 0) - Number(counts[slot] || 0))
    ), 0);
  }

  function activeLeagueId() {
    return String(root.getLeagueState?.()?.id || '');
  }

  function canonicalRoster() {
    return cloneRoster(root.getRoster?.() || []);
  }

  function workingRoster() {
    const leagueId = activeLeagueId();
    if (stagedRoster && stagedLeagueId === leagueId) return cloneRoster(stagedRoster);
    stagedRoster = null;
    stagedLeagueId = '';
    return canonicalRoster();
  }

  function hasPendingChanges(roster = workingRoster()) {
    return rosterSignature(roster) !== rosterSignature(canonicalRoster());
  }

  function resetStagedLineup() {
    stagedRoster = null;
    stagedLeagueId = '';
  }

  function escapeHtml(value = '') {
    if (typeof root.escapeHtml === 'function') return root.escapeHtml(value);
    return String(value)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }

  function formatKickoff(value = '') {
    const date = new Date(value);
    if (!value || Number.isNaN(date.getTime())) return '';
    return date.toLocaleString([], { weekday: 'short', hour: 'numeric', minute: '2-digit' });
  }

  function activeLock(player, context = currentWeekContext()) {
    return lockMap().get(String(player?.id || '')) || {
      playerId: player?.id,
      season: context.season,
      week: context.week,
      locked: player?.locked === true || player?.gameStarted === true,
      gameStartTime: player?.gameStartTime || ''
    };
  }

  async function refreshLockState(force = false) {
    const league = root.getLeagueState?.();
    const fallbackContext = currentWeekContext();
    if (!league?.id || !root.getAuthState?.()?.token || root.isLocalDemoSession?.()) {
      lockState = { leagueId: league?.id || '', season: fallbackContext.season, week: fallbackContext.week, weekLocked: false, players: [] };
      lockStateAt = Date.now();
      return lockState;
    }
    const currentMatches = lockState && String(lockState.leagueId || '') === String(league.id);
    if (!force && currentMatches && Date.now() - lockStateAt < LOCK_CACHE_MS) return lockState;
    if (lockRequest) return lockRequest;
    lockRequest = root.apiRequest(`/leagues/${encodeURIComponent(league.id)}/lineup-locks`)
      .then((state) => {
        lockState = state;
        lockStateAt = Date.now();
        return state;
      }).finally(() => { lockRequest = null; });
    return lockRequest;
  }

  function moveControl(player, roster, rules, context) {
    const locked = saving || playerLocked(player, lockState, context);
    const destinations = locked ? [] : legalDestinations(player, roster, rules);
    if (playerLocked(player, lockState, context)) return '<span class="lineup-lock-icon" aria-label="Player locked">🔒</span>';
    if (!destinations.length) return '<span class="lineup-move-placeholder" aria-hidden="true">—</span>';
    const options = destinations.map((slot) => `<option value="${slot}">Move to ${slot.toUpperCase()}</option>`).join('');
    return `<select class="lineup-move-select" data-lineup-player="${escapeHtml(player.id)}" aria-label="Move ${escapeHtml(player.name)}" ${saving ? 'disabled' : ''}>
      <option value="">Move</option>${options}</select>`;
  }

  function playerRow(player, slotLabel, roster, rules, context) {
    const lock = activeLock(player, context);
    const locked = playerLocked(player, lockState, context);
    const kickoff = formatKickoff(lock.gameStartTime);
    return `<div class="lineup-player-row${locked ? ' is-locked' : ''}">
      <div class="lineup-player-row__action">${moveControl(player, roster, rules, context)}</div>
      <div class="lineup-player-row__slot">${escapeHtml(slotLabel)}</div>
      <div class="lineup-player-row__identity"><strong>${escapeHtml(player.name)}</strong><span>${escapeHtml(player.position)} · ${escapeHtml(player.team)}</span></div>
      <div class="lineup-player-row__status">${locked ? '<span class="pill lineup-lock-pill">Locked</span>' : ''}${kickoff ? `<span>${escapeHtml(kickoff)}</span>` : ''}</div>
      <div class="lineup-player-row__points">${Number(player.projection || 0).toFixed(1)} pts</div>
    </div>`;
  }

  function emptyRow(slotLabel) {
    return `<div class="lineup-player-row lineup-player-row--empty">
      <div class="lineup-player-row__action"><span class="lineup-move-placeholder" aria-hidden="true">—</span></div>
      <div class="lineup-player-row__slot">${escapeHtml(slotLabel)}</div>
      <div class="lineup-player-row__identity"><strong>Empty starter slot</strong><span>This position will score zero points.</span></div>
      <div class="lineup-player-row__status"><span class="pill pill--muted">Empty</span></div>
      <div class="lineup-player-row__points">0.0 pts</div>
    </div>`;
  }

  function renderTeamPanel() {
    const rosterHost = root.document?.getElementById?.('team-roster');
    const slotsHost = root.document?.getElementById?.('team-slots');
    if (!rosterHost || !slotsHost) return;

    const roster = workingRoster();
    const rules = normalizedRules();
    const context = currentWeekContext();
    const grouped = groupRoster(roster, rules);
    const assigned = new Map();
    const starterHtml = starterSlots(rules).map((definition) => {
      const used = assigned.get(definition.slot) || 0;
      const player = grouped.startersBySlot.get(definition.slot)?.[used];
      assigned.set(definition.slot, used + 1);
      return player ? playerRow(player, definition.label, roster, rules, context) : emptyRow(definition.label);
    }).join('');
    const benchHtml = grouped.bench.length
      ? grouped.bench.map((player, index) => playerRow(player, `BN ${index + 1}`, roster, rules, context)).join('')
      : '<div class="lineup-empty-bench">No bench players.</div>';
    const dirty = hasPendingChanges(roster);
    const errors = lineupSaveErrors(roster, root.getLeagueState?.());
    const status = saving ? 'Saving lineup…'
      : errors.length ? errors[0].message
        : dirty ? 'Unsaved lineup changes' : 'Lineup matches the server';

    rosterHost.innerHTML = `<div class="lineup-week-heading">
      <div><strong>Week ${context.week} lineup</strong><span>${escapeHtml(status)}</span></div>
      <div class="lineup-editor-actions">
        <button class="button button--ghost lineup-refresh-locks" type="button" ${saving ? 'disabled' : ''}>Refresh locks</button>
        <button class="button button--ghost lineup-discard" type="button" ${!dirty || saving ? 'disabled' : ''}>Discard</button>
        <button class="button button--primary lineup-save" type="button" ${!dirty || saving || errors.length ? 'disabled' : ''}>${saving ? 'Saving…' : 'Save lineup'}</button>
      </div>
    </div>
    <section class="lineup-section" aria-labelledby="lineup-starters-title"><h3 id="lineup-starters-title">Starting lineup</h3><div class="lineup-table">${starterHtml || '<div class="lineup-empty-bench">No starter slots configured.</div>'}</div></section>
    <section class="lineup-section" aria-labelledby="lineup-bench-title"><h3 id="lineup-bench-title">Bench</h3><div class="lineup-table">${benchHtml}</div></section>`;

    const empty = emptyStarterCount(roster, rules);
    const lockedPlayers = roster.filter((player) => playerLocked(player, lockState, context)).length;
    slotsHost.innerHTML = `<div class="lineup-summary-item"><span>Starter slots</span><strong>${starterSlots(rules).length}</strong></div>
      <div class="lineup-summary-item"><span>Empty starters</span><strong>${empty}</strong></div>
      <div class="lineup-summary-item"><span>Locked players</span><strong>${lockedPlayers}</strong></div>
      <div class="lineup-summary-item"><span>Save status</span><strong>${errors.length ? 'Fix lineup' : dirty ? 'Pending' : 'Saved'}</strong></div>
      <div class="lineup-zero-note">Moves are staged locally. Save commits the complete lineup once; invalid or locked saves leave the server roster unchanged.</div>`;

    if (!lockState || String(lockState.leagueId || '') !== activeLeagueId()) {
      void refreshLockState().then(() => renderTeamPanel()).catch(() => {});
    }
  }

  function stagePlayerMove(playerId, slot) {
    const roster = workingRoster();
    const player = roster.find((item) => String(item.id) === String(playerId));
    if (!player || !slot) return false;
    const context = currentWeekContext();
    if (playerLocked(player, lockState, context)) throw new Error("This player's game has started, so the player is locked for the week.");
    if (!legalDestinations(player, roster, normalizedRules()).includes(slot)) throw new Error('That player cannot move to the selected lineup slot.');
    stagedRoster = stageMoveInRoster(roster, playerId, slot);
    stagedLeagueId = activeLeagueId();
    renderTeamPanel();
    return true;
  }

  async function saveLineup() {
    if (saving) return false;
    const roster = workingRoster();
    const errors = lineupSaveErrors(roster, root.getLeagueState?.());
    if (errors.length) throw new Error(errors[0].message);
    if (!hasPendingChanges(roster)) return false;
    const context = currentWeekContext();
    const assignments = assignmentsFromRoster(roster);
    saving = true;
    renderTeamPanel();
    try {
      if (root.isLocalDemoSession?.() || !root.getAuthState?.()?.token) {
        if (typeof root.setRoster === 'function') root.setRoster(roster);
        else assignments.forEach((assignment) => root.setRosterSlot?.(assignment.playerId, assignment.slot));
      } else if (root.CFFRosterTransactions?.mutate) {
        const fingerprint = `${context.season}:${context.week}:${assignments.map((item) => `${item.playerId}:${item.slot}`).join('|')}`;
        await root.CFFRosterTransactions.mutate('lineup', { assignments, season: context.season, week: context.week }, fingerprint);
      } else {
        throw new Error('The server-authoritative lineup service is unavailable.');
      }
      resetStagedLineup();
      await refreshLockState(true).catch(() => null);
      await root.refreshLeagueDashboard?.({ allowCached: false });
      root.CFF_UI?.notify?.('Lineup saved.', 'success');
      return true;
    } catch (error) {
      resetStagedLineup();
      throw error;
    } finally {
      saving = false;
      root.renderLeague?.();
      renderTeamPanel();
    }
  }

  function install() {
    installAttempts += 1;
    const ready = typeof root.getRoster === 'function'
      && typeof root.getLeagueState === 'function'
      && typeof root.renderLeague === 'function'
      && root.document?.getElementById?.('team-roster');
    if (!ready) {
      if (installAttempts < 500) root.setTimeout?.(install, 10);
      return false;
    }
    if (root.renderTeamPanel?.__cffWeeklyLineup) return true;

    root.lineupErrors = lineupErrorsAllowEmpty;
    root.lineupValid = (roster = root.getRoster?.() || [], league = root.getLeagueState?.()) => lineupErrorsAllowEmpty(roster, league).length === 0;
    renderTeamPanel.__cffWeeklyLineup = true;
    root.renderTeamPanel = renderTeamPanel;

    const rosterHost = root.document.getElementById('team-roster');
    rosterHost.addEventListener('change', (event) => {
      const select = event.target.closest?.('[data-lineup-player]');
      if (!select || !select.value) return;
      try { stagePlayerMove(select.dataset.lineupPlayer, select.value); }
      catch (error) {
        root.CFF_UI?.notify?.(error?.message || 'Lineup move failed.', 'error');
        renderTeamPanel();
      }
    });
    rosterHost.addEventListener('click', (event) => {
      if (event.target.closest?.('.lineup-save')) {
        void saveLineup().catch((error) => {
          root.CFF_UI?.notify?.(error?.userMessage || error?.data?.error || error?.message || 'Lineup save failed.', 'error');
        });
        return;
      }
      if (event.target.closest?.('.lineup-discard')) {
        resetStagedLineup();
        renderTeamPanel();
        return;
      }
      if (event.target.closest?.('.lineup-refresh-locks')) {
        void refreshLockState(true).then(() => renderTeamPanel()).catch((error) => {
          root.CFF_UI?.notify?.(error?.message || 'Could not refresh lineup locks.', 'error');
        });
      }
    });
    root.document.getElementById('scoreboard-week')?.addEventListener('change', () => {
      resetStagedLineup();
      lockStateAt = 0;
      void refreshLockState(true).then(() => renderTeamPanel()).catch(() => renderTeamPanel());
    });
    root.addEventListener?.('cff:roster-transaction', () => {
      if (!saving) resetStagedLineup();
      void refreshLockState(true).then(() => renderTeamPanel()).catch(() => renderTeamPanel());
    });
    root.addEventListener?.('cff:active-league-changed', () => {
      resetStagedLineup();
      lockState = null;
      lockStateAt = 0;
      renderTeamPanel();
    });
    root.addEventListener?.('online', () => { void refreshLockState(true).then(() => renderTeamPanel()).catch(() => {}); });
    root.document.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') void refreshLockState(true).then(() => renderTeamPanel()).catch(() => {});
    });

    root.CFFLineupManagement = Object.freeze({
      stage: stagePlayerMove,
      save: saveLineup,
      discard() { resetStagedLineup(); renderTeamPanel(); },
      pending: () => hasPendingChanges(),
      assignments: () => assignmentsFromRoster(workingRoster())
    });
    void refreshLockState(true).then(() => renderTeamPanel()).catch(() => renderTeamPanel());
    root.renderLeague();
    root.document.documentElement.dataset.cffLineupManagement = 'true';
    return true;
  }

  const helpers = {
    normalizedRules,
    starterSlots,
    cloneRoster,
    assignmentsFromRoster,
    rosterSignature,
    playerLocked,
    groupRoster,
    positionEligible,
    legalDestinations,
    lineupErrorsAllowEmpty,
    lineupSaveErrors,
    stageMoveInRoster,
    emptyStarterCount
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  root.setTimeout?.(install, 0);
})(typeof window !== 'undefined' ? window : globalThis);
