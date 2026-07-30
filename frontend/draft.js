const leagueNameEl = document.getElementById('draft-league-name');
const draftTypeLabel = document.getElementById('draft-type-label');
const rosterCount = document.getElementById('roster-count');
const queueCount = document.getElementById('queue-count');
const draftQueue = document.getElementById('draft-queue');
const rosterList = document.getElementById('roster-list');
const rosterBalance = document.getElementById('roster-balance');
const recommendedList = document.getElementById('recommended-list');
const clearDraftBtn = document.getElementById('clear-draft');
const draftPickList = document.getElementById('draft-pick-list');
const draftCurrentPick = document.getElementById('draft-current-pick');
const draftCurrentManager = document.getElementById('draft-current-manager');
const draftClock = document.getElementById('draft-clock');
const draftStatus = document.getElementById('draft-status');
const draftLocked = document.getElementById('draft-locked');
const draftLockedMessage = document.getElementById('draft-locked-message');
const draftLockedPrimary = document.getElementById('draft-locked-primary');
const draftRoomContent = document.getElementById('draft-room-content');
let draftTimer = null;
let autoPickInFlight = false;

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
  if (clearDraftBtn) clearDraftBtn.disabled = !isCurrentCommissioner(league);
  return canEnter;
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
        <button class="button button--primary" data-draft="${player.id}" type="button" ${myTurn && !complete ? '' : 'disabled'}>${complete ? 'Complete' : myTurn ? 'Draft' : 'Waiting'}</button>
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
      } catch {
        draftPlayer(player);
      }
      renderAll();
    });
  });
  draftQueue.querySelectorAll('[data-remove]').forEach((button) => {
    button.addEventListener('click', async () => {
      removeFromQueue(button.dataset.remove);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // Local queue remains updated.
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
      } catch {
        setRoster(getRoster().filter((item) => item.id !== player.id));
        addPlayerToQueue(player);
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
      addPlayerToQueue(player);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // Local queue remains updated.
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
  if (draftCurrentPick) {
    draftCurrentPick.textContent = meta.status === 'complete' ? 'Complete' : `Pick ${meta.currentPick || picks.length + 1}`;
  }
  if (draftCurrentManager) draftCurrentManager.textContent = meta.status === 'complete' ? 'Draft complete' : managerDisplayName(manager) || 'Manager TBD';
  if (draftStatus) draftStatus.textContent = meta.status === 'complete' ? 'Complete' : isMyDraftTurn(meta) ? 'Your pick' : 'Waiting';
  renderDraftClock();
  if (!draftPickList) return;
  if (!picks.length) {
    draftPickList.textContent = 'No picks made yet.';
    return;
  }
  draftPickList.innerHTML = picks.map((pick) => {
    const player = pick.player || {};
    return `
      <div class="row">
        <div>
          <strong>${pick.pickNumber}. ${player.name || 'Unknown player'}</strong>
          <div class="muted">${player.team || 'Team TBD'} ${player.position || ''} / ${escapeHtml(managerDisplayName(pick.managerEmail))}</div>
        </div>
        <span class="badge">${Number(player.projection || 0).toFixed(1)}</span>
      </div>
    `;
  }).join('');
}

function renderDraftClock() {
  if (!canEnterDraftRoom()) return;
  const meta = getDraftMeta();
  const remaining = draftClockRemaining(meta);
  if (draftClock) {
    draftClock.textContent = meta.status === 'complete' ? 'Done' : `${remaining}s`;
  }
}

async function maybeAutoPick() {
  if (!canEnterDraftRoom()) return;
  const meta = getDraftMeta();
  if (autoPickInFlight || meta.status === 'complete' || !isMyDraftTurn(meta) || draftClockRemaining(meta) > 0) return;
  autoPickInFlight = true;
  try {
    await autoPickFromQueueApi();
    await refreshDraftFromApi();
  } catch {
    try {
      await autoPickFromQueueApi();
    } catch {
      // No available auto-pick target.
    }
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
  if (!canEnterDraftRoom()) return;
  if (!getAuthState()?.token) return;
  try {
    await syncLeaguesFromApi();
    await syncActiveLeagueCollectionsFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep local draft controls responsive when the API is offline.
  }
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
  renderQueue();
  renderRoster();
  renderDraftPicks();
  renderRecommended();
}

clearDraftBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner()) return;
  try {
    await resetDraftApi();
  } catch {
    clearDraftState();
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
}

initDraftPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_LEAGUE_KEY, CFF_LEAGUES_KEY, CFF_QUEUE_KEY, CFF_ROSTER_KEY, CFF_DRAFT_PICKS_KEY, CFF_DRAFT_META_KEY].includes(event.key)) {
    renderAll();
  }
});
