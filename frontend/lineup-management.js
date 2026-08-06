(function initLineupManagement(root) {
  'use strict';

  const STARTER_ORDER = Object.freeze(['qb', 'rb', 'wr', 'te', 'flex', 'k', 'def']);
  const LOCK_CACHE_MS = 15000;
  let lockState = null;
  let lockStateAt = 0;
  let lockRequest = null;
  let installAttempts = 0;

  function normalizedRules(league = root.getLeagueState?.()) {
    return {
      qb: 1,
      rb: 2,
      wr: 2,
      te: 1,
      flex: 2,
      bench: 6,
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

  function currentWeekContext() {
    const leagueId = String(root.getLeagueState?.()?.id || '');
    if (lockState
        && String(lockState.leagueId || '') === leagueId
        && Number(lockState.season) > 0
        && Number(lockState.week) > 0) {
      return {
        season: Number(lockState.season),
        week: Number(lockState.week)
      };
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
        && Number(serverLock.week) === Number(context.week)) {
      return serverLock.locked === true;
    }
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
      return positionEligible(player, slot)
        && Number(occupied[slot] || 0) < Number(rules[slot] || 0);
    });
  }

  function lineupErrorsAllowEmpty(roster = root.getRoster?.() || [], league = root.getLeagueState?.()) {
    const rules = normalizedRules(league);
    const counts = slotOccupancy(roster);
    const errors = [];
    roster.forEach((player) => {
      const slot = String(player?.rosterSlot || 'bench').toLowerCase();
      if (slot !== 'bench' && !positionEligible(player, slot)) {
        errors.push({
          slot,
          playerId: String(player?.id || ''),
          message: `${player?.name || 'Player'} is not eligible for ${slot.toUpperCase()}`
        });
      }
    });
    STARTER_ORDER.forEach((slot) => {
      const limit = Number(rules?.[slot] || 0);
      const filled = Number(counts[slot] || 0);
      if (filled > limit) {
        errors.push({ slot, message: `Too many ${slot.toUpperCase()} starters` });
      }
    });
    const benchLimit = Number(rules.bench || 0);
    if (Number(counts.bench || 0) > benchLimit) {
      errors.push({ slot: 'bench', message: 'Too many bench players' });
    }
    return errors;
  }

  function emptyStarterCount(roster = [], rules = normalizedRules()) {
    const counts = slotOccupancy(roster);
    return STARTER_ORDER.reduce((total, slot) => (
      total + Math.max(0, Number(rules?.[slot] || 0) - Number(counts[slot] || 0))
    ), 0);
  }

  function escapeHtml(value = '') {
    if (typeof root.escapeHtml === 'function') return root.escapeHtml(value);
    return String(value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function formatKickoff(value = '') {
    const date = new Date(value);
    if (!value || Number.isNaN(date.getTime())) return '';
    return date.toLocaleString([], {
      weekday: 'short',
      hour: 'numeric',
      minute: '2-digit'
    });
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
      lockState = {
        leagueId: league?.id || '',
        season: fallbackContext.season,
        week: fallbackContext.week,
        weekLocked: false,
        players: []
      };
      lockStateAt = Date.now();
      return lockState;
    }
    const currentMatches = lockState
      && String(lockState.leagueId || '') === String(league.id);
    if (!force && currentMatches && Date.now() - lockStateAt < LOCK_CACHE_MS) return lockState;
    if (lockRequest) return lockRequest;
    lockRequest = root.apiRequest(
      `/leagues/${encodeURIComponent(league.id)}/lineup-locks`
    ).then((state) => {
      lockState = state;
      lockStateAt = Date.now();
      return state;
    }).finally(() => {
      lockRequest = null;
    });
    return lockRequest;
  }

  function moveControl(player, roster, rules, context) {
    const locked = playerLocked(player, lockState, context);
    const destinations = locked ? [] : legalDestinations(player, roster, rules);
    if (locked) {
      return '<span class="lineup-lock-icon" aria-label="Player locked">🔒</span>';
    }
    if (!destinations.length) {
      return '<span class="lineup-move-placeholder" aria-hidden="true">—</span>';
    }
    const options = destinations.map((slot) => (
      `<option value="${slot}">Move to ${slot.toUpperCase()}</option>`
    )).join('');
    return `
      <select class="lineup-move-select" data-lineup-player="${escapeHtml(player.id)}"
              aria-label="Move ${escapeHtml(player.name)}">
        <option value="">Move</option>
        ${options}
      </select>`;
  }

  function playerRow(player, slotLabel, roster, rules, context) {
    const lock = activeLock(player, context);
    const locked = playerLocked(player, lockState, context);
    const kickoff = formatKickoff(lock.gameStartTime);
    return `
      <div class="lineup-player-row${locked ? ' is-locked' : ''}">
        <div class="lineup-player-row__action">${moveControl(player, roster, rules, context)}</div>
        <div class="lineup-player-row__slot">${escapeHtml(slotLabel)}</div>
        <div class="lineup-player-row__identity">
          <strong>${escapeHtml(player.name)}</strong>
          <span>${escapeHtml(player.position)} · ${escapeHtml(player.team)}</span>
        </div>
        <div class="lineup-player-row__status">
          ${locked ? '<span class="pill lineup-lock-pill">Locked</span>' : ''}
          ${kickoff ? `<span>${escapeHtml(kickoff)}</span>` : ''}
        </div>
        <div class="lineup-player-row__points">${Number(player.projection || 0).toFixed(1)} pts</div>
      </div>`;
  }

  function emptyRow(slotLabel) {
    return `
      <div class="lineup-player-row lineup-player-row--empty">
        <div class="lineup-player-row__action"><span class="lineup-move-placeholder" aria-hidden="true">—</span></div>
        <div class="lineup-player-row__slot">${escapeHtml(slotLabel)}</div>
        <div class="lineup-player-row__identity">
          <strong>Empty starter slot</strong>
          <span>This position will score zero points.</span>
        </div>
        <div class="lineup-player-row__status"><span class="pill pill--muted">Empty</span></div>
        <div class="lineup-player-row__points">0.0 pts</div>
      </div>`;
  }

  function renderTeamPanel() {
    const rosterHost = root.document?.getElementById?.('team-roster');
    const slotsHost = root.document?.getElementById?.('team-slots');
    if (!rosterHost || !slotsHost) return;

    const roster = root.getRoster?.() || [];
    const rules = normalizedRules();
    const context = currentWeekContext();
    const grouped = groupRoster(roster, rules);
    const assigned = new Map();
    const starterHtml = starterSlots(rules).map((definition) => {
      const used = assigned.get(definition.slot) || 0;
      const player = grouped.startersBySlot.get(definition.slot)?.[used];
      assigned.set(definition.slot, used + 1);
      return player
        ? playerRow(player, definition.label, roster, rules, context)
        : emptyRow(definition.label);
    }).join('');
    const benchHtml = grouped.bench.length
      ? grouped.bench.map((player, index) => playerRow(player, `BN ${index + 1}`, roster, rules, context)).join('')
      : '<div class="lineup-empty-bench">No bench players.</div>';

    rosterHost.innerHTML = `
      <div class="lineup-week-heading">
        <div>
          <strong>Week ${context.week} lineup</strong>
          <span>Players lock individually when their college game begins.</span>
        </div>
        <button class="button button--ghost lineup-refresh-locks" type="button">Refresh locks</button>
      </div>
      <section class="lineup-section" aria-labelledby="lineup-starters-title">
        <h3 id="lineup-starters-title">Starting lineup</h3>
        <div class="lineup-table">${starterHtml || '<div class="lineup-empty-bench">No starter slots configured.</div>'}</div>
      </section>
      <section class="lineup-section" aria-labelledby="lineup-bench-title">
        <h3 id="lineup-bench-title">Bench</h3>
        <div class="lineup-table">${benchHtml}</div>
      </section>`;

    const empty = emptyStarterCount(roster, rules);
    const lockedPlayers = roster.filter((player) => playerLocked(player, lockState, context)).length;
    const errors = lineupErrorsAllowEmpty(roster, root.getLeagueState?.());
    slotsHost.innerHTML = `
      <div class="lineup-summary-item"><span>Starter slots</span><strong>${starterSlots(rules).length}</strong></div>
      <div class="lineup-summary-item"><span>Empty starters</span><strong>${empty}</strong></div>
      <div class="lineup-summary-item"><span>Locked players</span><strong>${lockedPlayers}</strong></div>
      <div class="lineup-summary-item"><span>Lineup status</span><strong>${errors.length ? 'Needs correction' : 'Valid'}</strong></div>
      <div class="lineup-zero-note">Empty starter positions are allowed and score 0 points for the week.</div>`;

    if (!lockState
        || String(lockState.leagueId || '') !== String(root.getLeagueState?.()?.id || '')) {
      void refreshLockState().then(() => renderTeamPanel()).catch(() => {});
    }
  }

  async function movePlayer(playerId, slot) {
    const roster = root.getRoster?.() || [];
    const player = roster.find((item) => String(item.id) === String(playerId));
    if (!player || !slot) return false;
    const context = currentWeekContext();
    if (playerLocked(player, lockState, context)) {
      throw new Error("This player's game has started, so the player is locked for the week.");
    }
    if (!legalDestinations(player, roster, normalizedRules()).includes(slot)) {
      throw new Error('That player cannot move to the selected lineup slot.');
    }

    if (root.isLocalDemoSession?.() || !root.getAuthState?.()?.token) {
      if (!root.setRosterSlot?.(playerId, slot)) throw new Error('The lineup move could not be saved.');
    } else if (root.CFFRosterTransactions?.mutate) {
      await root.CFFRosterTransactions.mutate(
        'slot',
        { playerId: String(playerId), slot, season: context.season, week: context.week },
        `${playerId}:${slot}:${context.season}:${context.week}`
      );
    } else {
      await root.updateRosterSlotApi?.(playerId, slot);
    }
    await refreshLockState(true).catch(() => null);
    root.renderLeague?.();
    return true;
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
    root.lineupValid = (roster = root.getRoster?.() || [], league = root.getLeagueState?.()) => (
      lineupErrorsAllowEmpty(roster, league).length === 0
    );
    renderTeamPanel.__cffWeeklyLineup = true;
    root.renderTeamPanel = renderTeamPanel;

    const rosterHost = root.document.getElementById('team-roster');
    rosterHost.addEventListener('change', (event) => {
      const select = event.target.closest?.('[data-lineup-player]');
      if (!select || !select.value) return;
      const playerId = select.dataset.lineupPlayer;
      const destination = select.value;
      select.disabled = true;
      void movePlayer(playerId, destination).catch((error) => {
        root.CFF_UI?.notify?.(error?.userMessage || error?.data?.error || error?.message || 'Lineup move failed.', 'error');
        renderTeamPanel();
      });
    });
    rosterHost.addEventListener('click', (event) => {
      if (!event.target.closest?.('.lineup-refresh-locks')) return;
      void refreshLockState(true).then(() => renderTeamPanel()).catch((error) => {
        root.CFF_UI?.notify?.(error?.message || 'Could not refresh lineup locks.', 'error');
      });
    });
    root.document.getElementById('scoreboard-week')?.addEventListener('change', () => {
      lockStateAt = 0;
      void refreshLockState(true).then(() => renderTeamPanel()).catch(() => renderTeamPanel());
    });
    root.addEventListener?.('cff:roster-transaction', () => {
      void refreshLockState(true).then(() => renderTeamPanel()).catch(() => renderTeamPanel());
    });
    root.addEventListener?.('online', () => {
      void refreshLockState(true).then(() => renderTeamPanel()).catch(() => {});
    });
    root.document.addEventListener?.('visibilitychange', () => {
      if (root.document.visibilityState === 'visible') {
        void refreshLockState(true).then(() => renderTeamPanel()).catch(() => {});
      }
    });

    void refreshLockState(true).then(() => renderTeamPanel()).catch(() => renderTeamPanel());
    root.renderLeague();
    root.document.documentElement.dataset.cffLineupManagement = 'true';
    return true;
  }

  const helpers = {
    normalizedRules,
    starterSlots,
    playerLocked,
    groupRoster,
    positionEligible,
    legalDestinations,
    lineupErrorsAllowEmpty,
    emptyStarterCount
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;
  root.setTimeout?.(install, 0);
})(typeof window !== 'undefined' ? window : globalThis);
