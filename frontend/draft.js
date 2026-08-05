const leagueNameEl = document.getElementById('draft-league-name');
const draftTypeLabel = document.getElementById('draft-type-label');
const rosterCount = document.getElementById('roster-count');
const queueCount = document.getElementById('queue-count');
const draftQueue = document.getElementById('draft-queue');
const rosterList = document.getElementById('roster-list');
const rosterBalance = document.getElementById('roster-balance');
const recommendedList = document.getElementById('recommended-list');
const clearDraftBtn = document.getElementById('clear-draft');
const undoLastPickBtn = document.getElementById('undo-last-pick');
const refreshDraftBtn = document.getElementById('refresh-draft');
const randomizeDraftOrderBtn = document.getElementById('randomize-draft-order');
const resetDraftOrderBtn = document.getElementById('reset-draft-order');
const draftOrderStatus = document.getElementById('draft-order-status');
const draftPickList = document.getElementById('draft-pick-list');
const draftCurrentPick = document.getElementById('draft-current-pick');
const draftCurrentManager = document.getElementById('draft-current-manager');
const draftClock = document.getElementById('draft-clock');
const draftStatus = document.getElementById('draft-status');
const draftRoundLabel = document.getElementById('draft-round-label');
const draftNextPickLabel = document.getElementById('draft-next-pick-label');
const draftOrderList = document.getElementById('draft-order-list');
const draftOrderCount = document.getElementById('draft-order-count');
const upcomingPickList = document.getElementById('upcoming-pick-list');
const upcomingPickCount = document.getElementById('upcoming-pick-count');
const draftLocked = document.getElementById('draft-locked');
const draftLockedMessage = document.getElementById('draft-locked-message');
const draftLockedPrimary = document.getElementById('draft-locked-primary');
const draftRoomContent = document.getElementById('draft-room-content');
const draftLobbyBadge = document.getElementById('draft-lobby-badge');
const draftLobbyCopy = document.getElementById('draft-lobby-copy');
const draftStartBtn = document.getElementById('draft-start');
const draftLobbyMembers = document.getElementById('draft-lobby-members');
const draftScheduledTime = document.getElementById('draft-scheduled-time');
const draftActivityLog = document.getElementById('draft-activity-log');
let draftTimer = null;
let autoPickInFlight = false;
let draftSyncTimer = null;
let draftRefreshInFlight = false;

function canEnterDraftRoom(league = getLeagueState()) {
  return Boolean(league && (league.draftLobbyOpen || isCurrentCommissioner(league)));
}

function draftLockedCopy(league = getLeagueState()) {
  if (!getAuthState()) return 'Sign in to enter this league draft room.';
  if (!league) return 'Select or join a league before entering a draft room.';
  if (!league.draftLobbyOpen) return 'The commissioner has not opened this draft room yet.';
  return '';
}

function renderDraftAccess() {
  const league = getLeagueState();
  const canEnter = canEnterDraftRoom(league);
  if (draftLocked) draftLocked.hidden = canEnter;
  if (draftRoomContent) draftRoomContent.hidden = !canEnter;
  if (draftLockedMessage) draftLockedMessage.textContent = draftLockedCopy(league);
  if (draftLockedPrimary) {
    draftLockedPrimary.textContent = getAuthState() ? 'League settings' : 'Sign in';
    draftLockedPrimary.href = getAuthState() ? 'league.html#settings' : 'signin.html';
  }
  const commissioner = isCurrentCommissioner(league);
  if (clearDraftBtn) clearDraftBtn.disabled = !commissioner;
  if (undoLastPickBtn) undoLastPickBtn.disabled = !commissioner || !getDraftPicks().length;
  const orderLocked = getDraftPicks().length > 0 || getDraftMeta().status !== 'not_started';
  if (randomizeDraftOrderBtn) randomizeDraftOrderBtn.disabled = !commissioner || orderLocked;
  if (resetDraftOrderBtn) resetDraftOrderBtn.disabled = !commissioner || orderLocked;
  return canEnter;
}

function renderDraftLobbyState() {
  const league = getLeagueState();
  const meta = getDraftMeta();
  const commissioner = isCurrentCommissioner(league);
  const activeManagers = (league?.members || []).filter((member) => String(member.status || '').toLowerCase() === 'active');
  const waiting = meta.status === 'not_started';
  const live = meta.status === 'open';
  const lobbyOpen = Boolean(meta.lobbyOpen || window.effectiveDraftLobbyOpen?.(league));
  const autoOpen = Boolean(!league?.draftLobbyOpen && window.draftLobbyAutoOpen?.(league));
  if (draftLobbyMembers) {
    draftLobbyMembers.textContent = `${activeManagers.length} active manager${activeManagers.length === 1 ? '' : 's'}`;
  }
  if (draftScheduledTime) {
    draftScheduledTime.textContent = league?.draftDate
      ? `Scheduled ${new Date(league.draftDate).toLocaleString()}. Lobby opens automatically ${window.DRAFT_LOBBY_AUTO_OPEN_MINUTES || 30} minutes before.`
      : 'Draft time not scheduled.';
  }
  if (draftLobbyBadge) {
    draftLobbyBadge.textContent = meta.status === 'complete' ? 'Complete' : live ? 'Live' : 'Lobby';
  }
  if (draftLobbyCopy) {
    if (!lobbyOpen) {
      draftLobbyCopy.textContent = commissioner
        ? 'Open the lobby from league settings, or it will open automatically before the scheduled draft time.'
        : 'The draft room opens automatically before the scheduled draft time.';
    } else if (meta.status === 'complete') {
      draftLobbyCopy.textContent = 'The draft is complete. The commissioner can reset it for another test.';
    } else if (live) {
      draftLobbyCopy.textContent = `Draft started${meta.startedAt ? ` ${new Date(meta.startedAt).toLocaleString()}` : ''}. Picks refresh automatically for every manager.`;
    } else if (autoOpen) {
      draftLobbyCopy.textContent = 'The draft lobby is open automatically. Mark ready before the commissioner starts.';
    } else if (commissioner && activeManagers.length < 2) {
      draftLobbyCopy.textContent = 'At least two active managers are required before the draft can start.';
    } else if (commissioner) {
      draftLobbyCopy.textContent = 'Managers may enter now. Start the draft when everyone is ready.';
    } else {
      draftLobbyCopy.textContent = 'You are in the lobby. Waiting for the commissioner to start the draft.';
    }
  }
  if (draftStartBtn) {
    draftStartBtn.hidden = !commissioner || !waiting;
    draftStartBtn.disabled = !lobbyOpen || activeManagers.length < 2;
  }
}

function renderLeagueHeader() {
  const league = getLeagueState();
  if (leagueNameEl) leagueNameEl.textContent = league?.name || 'No league selected';
  if (draftTypeLabel) draftTypeLabel.textContent = league?.draftTypeLabel || league?.draftType || 'Snake';
}

function renderQueue() {
  if (!canEnterDraftRoom()) return;
  const queue = getQueue();
  const meta = getDraftMeta();
  const myTurn = isMyDraftTurn(meta);
  const complete = meta.status === 'complete';
  const live = meta.status === 'open';
  if (queueCount) queueCount.textContent = `${queue.length} queued`;
  if (!draftQueue) return;
  if (!queue.length) {
    draftQueue.innerHTML = `
      <div class="row">
        <div>
          <strong>No queued players</strong>
          <div class="muted">Use player search or the recommended board to add targets.</div>
        </div>
        <a class="button" href="players.html">Find players</a>
      </div>
    `;
    return;
  }
  draftQueue.innerHTML = queue.map((player, index) => `
    <div class="row">
      <div>
        <strong>${index + 1}. ${player.name}</strong>
        <div class="muted">${player.team} ${player.position} / ${Number(player.projection).toFixed(1)} proj</div>
      </div>
      <div class="actions">
        <button class="button button--primary" data-draft="${player.id}" type="button" ${live && myTurn && !complete ? '' : 'disabled'}>${complete ? 'Complete' : !live ? 'Not started' : myTurn ? 'Draft' : 'Waiting'}</button>
        <button class="button button--ghost" data-remove="${player.id}" type="button">Remove</button>
      </div>
    </div>
  `).join('');

  draftQueue.querySelectorAll('[data-draft]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = getQueue().find((item) => item.id === button.dataset.draft);
      if (!player) return;
      try {
        await draftPlayerApi(player);
      } catch (error) {
        if (draftStatus) draftStatus.textContent = mutationErrorMessage(error, 'Could not draft player. No local changes were made.');
      }
      renderAll();
    });
  });
  draftQueue.querySelectorAll('[data-remove]').forEach((button) => {
    button.addEventListener('click', async () => {
      const playerId = button.dataset.remove;
      const nextQueue = getQueue().filter((item) => item.id !== playerId);
      try {
        await saveDraftQueueApi(nextQueue);
        setQueue(nextQueue);
      } catch (error) {
        if (draftStatus) draftStatus.textContent = mutationErrorMessage(error, 'Could not update draft queue. No local changes were made.');
      }
      renderAll();
    });
  });
}

function renderRoster() {
  if (!canEnterDraftRoom()) return;
  const roster = getRoster();
  if (rosterCount) rosterCount.textContent = `${roster.length} / ${rosterProjection().toFixed(1)} pts`;
  if (!rosterList) return;
  if (!roster.length) {
    rosterList.textContent = 'No players drafted yet.';
    renderRosterBalance();
    return;
  }
  rosterList.innerHTML = roster.map((player, index) => `
    <div class="row">
      <div>
        <strong>${(player.rosterSlot || player.position).toUpperCase()} - ${player.name}</strong>
        <div class="muted">${player.team} / ${player.conference} / ${player.position} / Pick ${index + 1}</div>
      </div>
      <div class="actions">
        <span class="badge">${Number(player.projection).toFixed(1)}</span>
        <button class="button button--ghost" data-release="${player.id}" type="button">Release</button>
      </div>
    </div>
  `).join('');
  rosterList.querySelectorAll('[data-release]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = getRoster().find((item) => item.id === button.dataset.release);
      if (!player) return;
      try {
        await releaseDraftedPlayerApi(player.id);
      } catch (error) {
        if (draftStatus) draftStatus.textContent = mutationErrorMessage(error, 'Could not release player. No local changes were made.');
      }
      renderAll();
    });
  });
  renderRosterBalance();
}

function renderRosterBalance() {
  if (!rosterBalance) return;
  const counts = getRoster().reduce((acc, player) => {
    const slot = String(player.rosterSlot || player.position || 'bench').toLowerCase();
    acc[slot] = (acc[slot] || 0) + 1;
    return acc;
  }, {});
  const league = getLeagueState();
  const rules = league?.rosterRules || defaultRosterRules;
  rosterBalance.innerHTML = ['qb', 'rb', 'wr', 'te', 'flex', 'bench'].map((slot) => `
    <div>
      <div class="label">${slot.toUpperCase()}</div>
      <div class="value">${counts[slot] || 0} / ${rules[slot]}</div>
    </div>
  `).join('');
}

function renderRecommended() {
  if (!canEnterDraftRoom()) return;
  if (!recommendedList) return;
  const usedIds = new Set([...getQueue(), ...getRoster()].map((player) => player.id));
  const available = samplePlayers.filter((player) => !usedIds.has(player.id)).slice(0, 8);
  if (!available.length) {
    recommendedList.textContent = 'Every recommended player is queued or drafted.';
    return;
  }
  recommendedList.innerHTML = available.map((player) => `
    <div class="row">
      <div>
        <strong>${player.name}</strong>
        <div class="muted">${player.team} ${player.position} / Rank ${player.rank}</div>
      </div>
      <button class="button" data-queue="${player.id}" type="button">Queue</button>
    </div>
  `).join('');
  recommendedList.querySelectorAll('[data-queue]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = samplePlayers.find((item) => item.id === button.dataset.queue);
      if (!player) return;
      const nextQueue = [...getQueue().filter((item) => item.id !== player.id), normalizePlayer(player)];
      try {
        await saveDraftQueueApi(nextQueue);
        setQueue(nextQueue);
      } catch (error) {
        if (draftStatus) draftStatus.textContent = mutationErrorMessage(error, 'Could not update draft queue. No local changes were made.');
      }
      renderAll();
    });
  });
}

function renderDraftPicks() {
  if (!canEnterDraftRoom()) return;
  const meta = getDraftMeta();
  const picks = getDraftPicks();
  const manager = currentDraftManager(meta);
  const order = draftOrderFromLeague();
  const currentPick = Number(meta.currentPick || picks.length + 1);
  const round = order.length ? Math.floor((Math.max(1, currentPick) - 1) / order.length) + 1 : 1;
  const waiting = meta.status === 'not_started';
  if (draftCurrentPick) {
    draftCurrentPick.textContent = meta.status === 'complete' ? 'Complete' : waiting ? 'Waiting' : `Pick ${currentPick}`;
  }
  if (draftCurrentManager) draftCurrentManager.textContent = meta.status === 'complete'
    ? 'Draft complete'
    : waiting
      ? 'Commissioner starts draft'
      : managerDisplayName(manager) || 'Manager TBD';
  if (draftStatus) draftStatus.textContent = meta.status === 'complete'
    ? 'Complete'
    : waiting
      ? 'Lobby'
      : isMyDraftTurn(meta) ? 'Your pick' : 'Waiting';
  if (draftRoundLabel) draftRoundLabel.textContent = meta.status === 'complete' ? 'Complete' : waiting ? 'Lobby' : `Round ${round}`;
  if (draftNextPickLabel) draftNextPickLabel.textContent = meta.status === 'complete' ? 'Done' : waiting ? 'Not started' : `Pick ${currentPick}`;
  renderDraftClock();
  if (!draftPickList) return;
  if (!picks.length) {
    draftPickList.textContent = 'No picks made yet.';
    return;
  }
  draftPickList.innerHTML = picks.map((pick) => {
    const player = pick.player || {};
    const last = Number(pick.pickNumber) === Number(picks[picks.length - 1]?.pickNumber);
    const automatic = Boolean(pick.automatic || (pick.selectionSource && pick.selectionSource !== 'manual'));
    const sourceLabel = pick.selectionSource === 'personal_queue'
      ? 'Queue auto-pick'
      : pick.selectionSource === 'system_ranking'
        ? 'System auto-pick'
        : automatic
          ? 'Auto-pick'
          : 'Manual';
    return `
      <div class="row">
        <div>
          <strong>${pick.pickNumber}. ${player.name || 'Unknown player'}</strong>
          <div class="muted">${player.team || 'Team TBD'} ${player.position || ''} / ${escapeHtml(managerDisplayName(pick.managerEmail))}</div>
        </div>
        <div class="actions">
          ${last ? '<span class="pill pill--muted">Last pick</span>' : ''}
          <span class="pill ${automatic ? '' : 'pill--muted'}">${sourceLabel}</span>
          <span class="badge">${Number(player.projection || 0).toFixed(1)}</span>
        </div>
      </div>
    `;
  }).join('');
}

function renderDraftActivityLog() {
  if (!draftActivityLog) return;
  const activity = Array.isArray(getDraftMeta()?.activity) ? getDraftMeta().activity : [];
  if (!activity.length) {
    draftActivityLog.textContent = 'Draft activity will appear here.';
    return;
  }
  draftActivityLog.innerHTML = activity.slice(0, 30).map((entry) => {
    const time = entry.createdAt ? new Date(entry.createdAt).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', second: '2-digit' }) : '';
    const manager = entry.managerEmail ? ` / ${escapeHtml(managerDisplayName(entry.managerEmail))}` : '';
    return `
      <div class="row">
        <div>
          <strong>${escapeHtml(time || 'Draft event')}</strong>
          <div class="muted">${escapeHtml(entry.message || entry.eventType || 'Draft event')}${manager}</div>
        </div>
        ${entry.pickNumber ? `<span class="badge">Pick ${Number(entry.pickNumber)}</span>` : '<span class="pill pill--muted">Log</span>'}
      </div>
    `;
  }).join('');
}

function renderUpcomingPicks() {
  if (!canEnterDraftRoom()) return;
  const meta = getDraftMeta();
  const picks = getDraftPicks();
  const order = draftOrderFromLeague();
  const currentPick = Number(meta.currentPick || picks.length + 1);
  const count = order.length ? Math.min(5, Math.max(3, order.length)) : 0;
  if (upcomingPickCount) upcomingPickCount.textContent = count ? `Next ${count}` : 'Order needed';
  if (!upcomingPickList) return;
  if (meta.status === 'complete') {
    upcomingPickList.textContent = 'Draft complete.';
    return;
  }
  if (!order.length) {
    upcomingPickList.textContent = 'Upcoming picks will appear when draft order is set.';
    return;
  }
  upcomingPickList.innerHTML = Array.from({ length: count }, (_, index) => {
    const pickNumber = currentPick + index;
    const manager = draftManagerForPick(order, pickNumber, meta.draftType || getLeagueState()?.draftType || 'snake');
    const round = Math.floor((pickNumber - 1) / order.length) + 1;
    return `
      <div class="row ${index === 0 ? 'row--active' : ''}">
        <div>
          <strong>Pick ${pickNumber}</strong>
          <div class="muted">Round ${round} / ${escapeHtml(managerDisplayName(manager))}</div>
        </div>
        ${index === 0 ? '<span class="badge">On clock</span>' : '<span class="pill pill--muted">Upcoming</span>'}
      </div>
    `;
  }).join('');
}

function draftOrderFromLeague(league = getLeagueState()) {
  const meta = getDraftMeta();
  if (Array.isArray(meta.draftOrder) && meta.draftOrder.length) return meta.draftOrder;
  return (league?.members || [])
    .filter((member) => String(member.status || '').toLowerCase() === 'active')
    .map((member) => member.email)
    .filter(Boolean);
}

function memberOrderFromLeague(league = getLeagueState()) {
  return (league?.members || [])
    .filter((member) => String(member.status || '').toLowerCase() === 'active')
    .map((member) => member.email)
    .filter(Boolean);
}

function shuffledOrder(order = []) {
  const next = [...order];
  for (let index = next.length - 1; index > 0; index -= 1) {
    const swap = Math.floor(Math.random() * (index + 1));
    [next[index], next[swap]] = [next[swap], next[index]];
  }
  return next;
}

function renderDraftOrder() {
  if (!canEnterDraftRoom()) return;
  const league = getLeagueState();
  const meta = getDraftMeta();
  const order = draftOrderFromLeague(league);
  const current = meta.status === 'complete' ? '' : currentDraftManager(meta);
  if (draftOrderCount) draftOrderCount.textContent = `${order.length} manager${order.length === 1 ? '' : 's'}`;
  if (!draftOrderList) return;
  if (!order.length) {
    draftOrderList.textContent = 'Draft order will appear when managers join.';
    return;
  }
  draftOrderList.innerHTML = order.map((email, index) => {
    const active = email === current;
    return `
      <div class="row ${active ? 'row--active' : ''}">
        <div>
          <strong>${index + 1}. ${escapeHtml(managerDisplayName(email))}</strong>
          <div class="muted">${escapeHtml(email)}</div>
        </div>
        ${active ? '<span class="badge">On clock</span>' : '<span class="pill pill--muted">Waiting</span>'}
      </div>
    `;
  }).join('');
  if (draftOrderStatus) {
    draftOrderStatus.textContent = getDraftPicks().length
      ? 'Draft order is locked after the first pick.'
      : isCurrentCommissioner(league)
        ? 'Set the order before the first pick.'
        : '';
  }
}

function renderDraftClock() {
  if (!canEnterDraftRoom()) return;
  const meta = getDraftMeta();
  const remaining = draftClockRemaining(meta);
  if (draftClock) {
    draftClock.textContent = meta.status === 'complete' ? 'Done' : meta.status !== 'open' ? 'Waiting' : `${remaining}s`;
  }
}

async function maybeAutoPick() {
  if (!canEnterDraftRoom()) return;
  const meta = getDraftMeta();
  if (autoPickInFlight || meta.status !== 'open' || draftClockRemaining(meta) > 0) return;
  autoPickInFlight = true;
  try {
    await refreshDraftFromApi();
  } catch {
    // Keep the last confirmed state; mutation controls are disabled by outage handling.
  } finally {
    autoPickInFlight = false;
    renderAll();
  }
}

function startDraftTimer() {
  if (draftTimer) clearInterval(draftTimer);
  draftTimer = setInterval(() => {
    renderDraftClock();
    maybeAutoPick();
  }, 1000);
}

async function refreshDraftFromApi() {
  if (!getAuthState()?.token) return;
  if (draftRefreshInFlight) return;
  draftRefreshInFlight = true;
  try {
    await syncLeaguesFromApi();
    await syncActiveLeagueCollectionsFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep the last authoritative draft snapshot visible during an outage.
  } finally {
    draftRefreshInFlight = false;
  }
}

function startDraftSyncPolling() {
  if (draftSyncTimer) clearInterval(draftSyncTimer);
  draftSyncTimer = setInterval(async () => {
    if (document.visibilityState !== 'visible' || !getAuthState()?.token) return;
    await refreshDraftFromApi();
    renderAll();
  }, 2000);
}

async function refreshDraftLeagueShell() {
  const requestedLeague = new URLSearchParams(window.location.search).get('league');
  if (requestedLeague) {
    setActiveLeague(requestedLeague);
  }
  if (!getAuthState()?.token) return;
  try {
    await syncLeaguesFromApi();
    if (requestedLeague) {
      setActiveLeague(requestedLeague);
    }
  } catch {
    // Use the current local league cache if the API is unavailable.
  }
}

function renderAll() {
  updateSharedNav('league');
  if (!renderDraftAccess()) return;
  renderLeagueHeader();
  renderDraftLobbyState();
  renderQueue();
  renderRoster();
  renderDraftOrder();
  renderDraftPicks();
  renderUpcomingPicks();
  renderRecommended();
  renderDraftActivityLog();
  renderDraftOutageState();
}

function renderDraftOutageState() {
  const meta = apiCacheMeta('league');
  const stale = Boolean(meta?.stale || mutationControlsDisabled());
  let banner = document.getElementById('draft-stale-warning');
  if (!stale) {
    banner?.remove();
    draftRoomContent?.querySelectorAll('[data-cff-outage-disabled="true"]').forEach((button) => {
      button.disabled = false;
      delete button.dataset.cffOutageDisabled;
    });
    return;
  }
  if (!banner) {
    banner = document.createElement('div');
    banner.id = 'draft-stale-warning';
    banner.className = 'notice notice--warning';
    draftRoomContent?.prepend(banner);
  }
  const fetched = meta?.fetchedAt ? ` Last server refresh: ${new Date(meta.fetchedAt).toLocaleString()}.` : '';
  banner.textContent = `Showing cached draft data because the API is unavailable. Draft mutations are disabled until the service recovers.${fetched}`;
  draftRoomContent?.querySelectorAll('button').forEach((button) => {
    if (button.disabled) return;
    button.disabled = true;
    button.dataset.cffOutageDisabled = 'true';
  });
}

draftStartBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner()) return;
  draftStartBtn.disabled = true;
  try {
    await startDraftApi();
    await refreshDraftFromApi();
  } catch (error) {
    if (draftLobbyCopy) draftLobbyCopy.textContent = mutationErrorMessage(error, 'Could not start the draft.');
  }
  renderAll();
});

refreshDraftBtn?.addEventListener('click', async () => {
  await refreshDraftFromApi();
  renderAll();
});

undoLastPickBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner()) return;
  try {
    await undoLastDraftPickApi();
  } catch (error) {
    if (draftOrderStatus) draftOrderStatus.textContent = mutationErrorMessage(error, 'Could not undo draft pick. No local changes were made.');
  }
  renderAll();
});

randomizeDraftOrderBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner() || getDraftPicks().length) return;
  const order = shuffledOrder(memberOrderFromLeague());
  if (!order.length) return;
  try {
    await saveDraftOrderApi(order);
    renderAll();
    if (draftOrderStatus) draftOrderStatus.textContent = 'Draft order randomized.';
  } catch (error) {
    renderAll();
    if (draftOrderStatus) draftOrderStatus.textContent = mutationErrorMessage(error, 'Could not randomize draft order. No local changes were made.');
  }
});

resetDraftOrderBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner() || getDraftPicks().length) return;
  const order = memberOrderFromLeague();
  if (!order.length) return;
  try {
    await saveDraftOrderApi(order);
    renderAll();
    if (draftOrderStatus) draftOrderStatus.textContent = 'Draft order reset.';
  } catch (error) {
    renderAll();
    if (draftOrderStatus) draftOrderStatus.textContent = mutationErrorMessage(error, 'Could not reset draft order. No local changes were made.');
  }
});

clearDraftBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner()) return;
  try {
    await resetDraftApi();
  } catch (error) {
    if (draftOrderStatus) draftOrderStatus.textContent = mutationErrorMessage(error, 'Could not reset draft. No local changes were made.');
  }
  renderAll();
});

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  window.location.href = 'index.html';
});

async function initDraftPage() {
  await validateAuthSession();
  renderAll();
  await refreshDraftLeagueShell();
  renderAll();
  await refreshDraftFromApi();
  renderAll();
  startDraftTimer();
  startDraftSyncPolling();
}

initDraftPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_LEAGUE_KEY, CFF_LEAGUES_KEY, CFF_QUEUE_KEY, CFF_ROSTER_KEY, CFF_DRAFT_PICKS_KEY, CFF_DRAFT_META_KEY].includes(event.key)) {
    renderAll();
  }
});
