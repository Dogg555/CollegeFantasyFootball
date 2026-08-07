const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO === true
  && ['localhost', '127.0.0.1', '::1'].includes(window.location.hostname);
const PAGE_SIZE = 50;
const directoryModel = window.CFFFreeAgentDirectory;

const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const positionFilter = document.getElementById('position-filter');
const conferenceFilter = document.getElementById('conference-filter');
const teamFilter = document.getElementById('team-filter');
const availabilityFilter = document.getElementById('availability-filter');
const opponentFilter = document.getElementById('opponent-filter');
const gameStatusFilter = document.getElementById('game-status-filter');
const searchResultsEl = document.getElementById('search-results');
const queueList = document.getElementById('queue-list');
const queueCount = document.getElementById('queue-count');
const playerDataStatus = document.getElementById('player-data-status');
const playerLeagueContext = document.getElementById('player-league-context');
const playerDirectoryMode = document.getElementById('player-directory-mode');
const playerCapabilityNote = document.getElementById('player-capability-note');
const loadMorePlayers = document.getElementById('load-more-players');
const playerResultCount = document.getElementById('player-result-count');
const playerMetaCount = document.getElementById('player-meta-count');
const playerMetaTeams = document.getElementById('player-meta-teams');
const playerMetaSeason = document.getElementById('player-meta-season');
const playerMetaUpdated = document.getElementById('player-meta-updated');
const previewCard = document.getElementById('free-agent-preview');
const previewPlayerEl = document.getElementById('free-agent-preview-player');
const previewRosterCount = document.getElementById('free-agent-roster-count');
const previewRosterAfter = document.getElementById('free-agent-roster-after');
const previewDestination = document.getElementById('free-agent-destination');
const previewDropField = document.getElementById('free-agent-drop-field');
const previewDropSelect = document.getElementById('free-agent-drop-select');
const previewRosterEl = document.getElementById('free-agent-preview-roster');
const previewMessage = document.getElementById('free-agent-preview-message');
const previewConfirm = document.getElementById('free-agent-confirm');
const previewCancel = document.getElementById('free-agent-cancel');

let lastResults = [];
let currentOffset = 0;
let hasMore = false;
let loadingPlayers = false;
let playerSearchQueued = false;
let activeLeague = null;
let directoryAvailable = false;
let directoryCapabilities = {};
let previewPlayer = null;
let previewLocks = [];
let previewWeekLocked = false;
let previewBusy = false;
let searchTimer = null;

function safeText(value, fallback = '') {
  return escapeHtml(value ?? fallback);
}

function safeNumber(value, fallback = 0) {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : fallback;
}

function refreshAuthState() {
  updateSharedNav('players');
}

function serverLeagueSelected() {
  return Boolean(getAuthState()?.token && activeLeague?.id && !isLocalDemoSession?.());
}

function rosterRules() {
  return activeLeague?.rosterRules || window.defaultRosterRules || { qb: 1, rb: 2, wr: 2, te: 1, flex: 2, bench: 6 };
}

function renderLeagueContext() {
  activeLeague = getLeagueState?.() || null;
  if (serverLeagueSelected()) {
    if (playerLeagueContext) playerLeagueContext.textContent = `League: ${activeLeague.name || activeLeague.id}`;
    if (playerDirectoryMode) playerDirectoryMode.textContent = 'League free agents';
  } else {
    if (playerLeagueContext) playerLeagueContext.textContent = getAuthState()?.token
      ? 'Select an active league to see availability and make roster moves.'
      : 'Sign in and select a league to add free agents.';
    if (playerDirectoryMode) playerDirectoryMode.textContent = 'Public player pool';
  }
}

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  activeLeague = null;
  directoryAvailable = false;
  closeTransactionPreview();
  refreshAuthState();
  renderLeagueContext();
  renderQueue();
  void loadPlayerPool();
});

async function fetchPublicPlayers(term = '', position = '', offset = 0) {
  const params = new URLSearchParams({ limit: String(PAGE_SIZE), offset: String(offset) });
  if (term) params.set('query', term);
  if (position) params.set('position', position);
  if (conferenceFilter?.value.trim()) params.set('conference', conferenceFilter.value.trim());
  if (teamFilter?.value.trim()) params.set('team', teamFilter.value.trim());
  const response = await fetch(`${apiBase}/players?${params.toString()}`, {
    headers: { Accept: 'application/json' },
  });
  if (!response.ok) throw new Error(`Player search failed with ${response.status}.`);
  const items = await response.json();
  return {
    items: items.map((player) => ({
      ...normalizePlayer(player),
      season: safeNumber(player.season, 0),
      updatedAt: player.updatedAt || '',
      availability: 'browse'
    })),
    capabilities: {},
    directAcquisitionAllowed: false,
    leagueScoped: false
  };
}

async function fetchLeaguePlayers(term = '', position = '', offset = 0) {
  if (!serverLeagueSelected()) return fetchPublicPlayers(term, position, offset);
  const params = new URLSearchParams({ limit: String(PAGE_SIZE), offset: String(offset) });
  if (term) params.set('query', term);
  if (position) params.set('position', position);
  if (conferenceFilter?.value.trim()) params.set('conference', conferenceFilter.value.trim());
  if (teamFilter?.value.trim()) params.set('team', teamFilter.value.trim());
  if (availabilityFilter?.value) params.set('availability', availabilityFilter.value);
  if (opponentFilter?.value.trim()) params.set('opponent', opponentFilter.value.trim());
  if (gameStatusFilter?.value) params.set('gameStatus', gameStatusFilter.value);
  return apiRequest(`/leagues/${encodeURIComponent(activeLeague.id)}/players?${params.toString()}`)
    .then((payload) => ({ ...payload, leagueScoped: true }));
}

async function fetchPlayerMeta() {
  const response = await fetch(`${apiBase}/players/meta`, { headers: { Accept: 'application/json' } });
  if (!response.ok) throw new Error(`Player metadata failed with ${response.status}.`);
  return response.json();
}

async function syncRosterForDirectory() {
  if (!serverLeagueSelected()) return null;
  if (typeof window.syncRosterTransactionState === 'function') {
    return window.syncRosterTransactionState();
  }
  const state = await apiRequest(`/leagues/${encodeURIComponent(activeLeague.id)}/roster/state`);
  setRoster((state?.roster || []).map((player) => normalizePlayer(player)));
  return state;
}

async function fetchRosterLocks() {
  if (!serverLeagueSelected()) return { players: [], weekLocked: false };
  return apiRequest(`/leagues/${encodeURIComponent(activeLeague.id)}/lineup-locks`);
}

function formatSyncTime(value) {
  if (!value) return 'Not synced';
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) return 'Recently';
  return new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' }).format(parsed);
}

function renderPlayerMeta(meta = {}) {
  if (playerMetaCount) playerMetaCount.textContent = Number(meta.activePlayers || 0).toLocaleString();
  if (playerMetaTeams) playerMetaTeams.textContent = Number(meta.teams || 0).toLocaleString();
  if (playerMetaSeason) playerMetaSeason.textContent = meta.season || '-';
  if (playerMetaUpdated) playerMetaUpdated.textContent = formatSyncTime(meta.lastUpdated);
  if (playerDataStatus) {
    playerDataStatus.textContent = meta.status === 'ok'
      ? `${Number(meta.activePlayers || 0).toLocaleString()} active players across ${Number(meta.teams || 0).toLocaleString()} teams`
      : 'Player catalog metadata unavailable';
  }
}

function renderCapabilities(capabilities = {}, leagueScoped = false) {
  directoryCapabilities = capabilities || {};
  if (!playerCapabilityNote) return;
  const notes = [];
  if (leagueScoped && capabilities.points === false) notes.push(capabilities.pointsNote || 'Fantasy-points filtering is not available yet.');
  if (leagueScoped && capabilities.projections === false) notes.push(capabilities.projectionsNote || 'Projection filtering is not available yet.');
  if (!leagueScoped) notes.push('League availability, opponent status, and roster actions appear after you sign in and select a league.');
  playerCapabilityNote.textContent = notes.join(' ');
  playerCapabilityNote.hidden = notes.length === 0;
}

async function loadPlayerMeta() {
  try {
    renderPlayerMeta(await fetchPlayerMeta());
  } catch {
    renderPlayerMeta({ status: 'unavailable' });
  }
}

async function loadPlayerPool({ append = false } = {}) {
  if (!searchResultsEl) return;
  if (loadingPlayers) {
    if (!append) playerSearchQueued = true;
    return;
  }
  loadingPlayers = true;
  const term = searchInput?.value.trim() || '';
  const position = positionFilter?.value || '';
  const offset = append ? currentOffset : 0;
  if (!append) searchResultsEl.textContent = term ? 'Searching current rosters...' : 'Loading current-season players...';
  if (loadMorePlayers) loadMorePlayers.disabled = true;

  try {
    const payload = await fetchLeaguePlayers(term, position, offset);
    if (playerSearchQueued) return;
    const batch = (payload.items || payload || []).map((player) => ({
      ...normalizePlayer(player),
      ...player,
      season: safeNumber(player.season, 0),
      updatedAt: player.updatedAt || ''
    }));
    directoryAvailable = payload.leagueScoped === true;
    renderCapabilities(payload.capabilities || {}, directoryAvailable);
    const combined = append ? [...lastResults, ...batch] : batch;
    const unique = new Map(combined.map((player) => [player.id, player]));
    lastResults = [...unique.values()];
    currentOffset = offset + batch.length;
    hasMore = batch.length === PAGE_SIZE;
    renderSearchResults(lastResults);
    if (playerResultCount) playerResultCount.textContent = `${lastResults.length.toLocaleString()} player${lastResults.length === 1 ? '' : 's'} shown`;
    if (loadMorePlayers) {
      loadMorePlayers.hidden = !hasMore;
      loadMorePlayers.disabled = false;
    }
  } catch (error) {
    if (playerSearchQueued) return;
    directoryAvailable = false;
    renderCapabilities({}, false);
    if (allowLocalDemo && !append) {
      lastResults = applyPositionFilter(filterSamplePlayers(term), position)
        .map((player) => ({ ...player, availability: 'browse' }));
      renderSearchResults(lastResults, true);
      if (playerDataStatus) playerDataStatus.textContent = 'Offline player preview';
      if (playerResultCount) playerResultCount.textContent = `${lastResults.length} preview players shown`;
    } else if (!append) {
      lastResults = [];
      searchResultsEl.textContent = serverLeagueSelected()
        ? 'League player availability is temporarily unavailable. No roster actions are enabled.'
        : 'The current player database is temporarily unavailable.';
      if (playerDataStatus) playerDataStatus.textContent = 'Player sync unavailable';
      if (playerResultCount) playerResultCount.textContent = '0 players shown';
      if (error?.status === 401 || error?.status === 403) renderLeagueContext();
    }
    if (loadMorePlayers) loadMorePlayers.hidden = true;
  } finally {
    loadingPlayers = false;
    if (playerSearchQueued) {
      playerSearchQueued = false;
      currentOffset = 0;
      void loadPlayerPool();
    }
  }
}

function schedulePlayerSearch() {
  window.clearTimeout(searchTimer);
  searchTimer = window.setTimeout(() => {
    currentOffset = 0;
    void loadPlayerPool();
  }, 250);
}

searchForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  currentOffset = 0;
  await loadPlayerPool();
});

[positionFilter, availabilityFilter, gameStatusFilter].forEach((control) => {
  control?.addEventListener('change', () => {
    currentOffset = 0;
    void loadPlayerPool();
  });
});
[searchInput, conferenceFilter, teamFilter, opponentFilter].forEach((control) => {
  control?.addEventListener('input', schedulePlayerSearch);
});
loadMorePlayers?.addEventListener('click', () => loadPlayerPool({ append: true }));

function applyPositionFilter(players, position) {
  if (!position) return players;
  return players.filter((player) => player.position === position);
}

function availabilityMetadata(player) {
  if (!directoryAvailable) return '';
  const parts = [String(player.availability || 'unknown').replaceAll('_', ' ')];
  if (player.opponent) parts.push(`vs ${player.opponent}`);
  if (player.gameStatus) parts.push(player.gameStatus);
  parts.push(`${safeNumber(player.rosterPercentage, 0).toFixed(1)}% rostered`);
  parts.push(player.points == null ? 'Points —' : `Points ${player.points}`);
  parts.push(player.projection == null ? 'Proj —' : `Proj ${player.projection}`);
  return parts.join(' / ');
}

function queueButtonMarkup(player, index, queuedIds) {
  const queued = queuedIds.has(player.id);
  return `<button class="button button--ghost" data-queue-player-index="${index}" type="button" ${queued ? 'disabled' : ''}>${queued ? 'Queued' : 'Queue'}</button>`;
}

function renderSearchResults(players = [], fallback = false) {
  if (!searchResultsEl) return;
  if (!players.length) {
    searchResultsEl.textContent = 'No active players matched those filters.';
    return;
  }
  const queuedIds = new Set(getQueue().map((player) => player.id));
  const notice = fallback
    ? '<div class="row"><div><strong>Offline player preview</strong><div class="muted">Showing preview players until the current roster database is reachable.</div></div></div>'
    : '';
  searchResultsEl.innerHTML = notice + players.map((player, index) => {
    const seasonLabel = player.season ? ` / ${safeNumber(player.season)}` : '';
    const action = directoryModel?.availabilityAction(player, directoryAvailable)
      || { label: directoryAvailable ? 'Add' : 'Select a league', enabled: directoryAvailable, action: 'add' };
    const availability = directoryAvailable
      ? `<span class="player-availability">${safeText(String(player.availability || 'unknown').replaceAll('_', ' '))}</span>`
      : '';
    return `
      <div class="row">
        <div>
          <strong>${safeText(player.name, 'Unknown player')}</strong> - ${safeText(player.team, 'Team TBD')} (${safeText(player.position, 'FLEX')})
          <div class="muted">${safeText(player.conference, 'Conference TBD')} / ${safeText(player.class, 'Class TBD')}${seasonLabel}</div>
          ${availability}
          ${directoryAvailable ? `<div class="muted">${safeText(availabilityMetadata(player))}</div>` : ''}
        </div>
        <div class="player-row-actions">
          <button class="button button--primary" data-add-player-index="${index}" type="button" ${action.enabled ? '' : 'disabled'}>${safeText(action.label)}</button>
          ${queueButtonMarkup(player, index, queuedIds)}
        </div>
      </div>
    `;
  }).join('');

  searchResultsEl.querySelectorAll('[data-add-player-index]').forEach((button) => {
    button.addEventListener('click', () => {
      const player = players[Number(button.dataset.addPlayerIndex)];
      if (player) void openTransactionPreview(player);
    });
  });
  searchResultsEl.querySelectorAll('[data-queue-player-index]').forEach((button) => {
    button.addEventListener('click', () => {
      const player = players[Number(button.dataset.queuePlayerIndex)];
      if (player) void addPlayerToQueue(player, button);
    });
  });
}

async function addPlayerToQueue(player, button) {
  const nextQueue = [...getQueue().filter((item) => item.id !== player.id), normalizePlayer(player)];
  button.disabled = true;
  try {
    await saveDraftQueueApi(nextQueue);
    setQueue(nextQueue);
    button.textContent = 'Queued';
    window.CFF_UI?.notify(`${player.name} added to your draft queue.`, 'success');
  } catch (error) {
    button.disabled = false;
    window.CFF_UI?.notify(mutationErrorMessage(error, 'Could not update draft queue. No local changes were made.'), 'error');
    return;
  }
  renderQueue();
}

function closeTransactionPreview() {
  previewPlayer = null;
  previewLocks = [];
  previewWeekLocked = false;
  previewBusy = false;
  if (previewCard) previewCard.hidden = true;
  if (previewDropSelect) previewDropSelect.innerHTML = '';
}

function dropPlayerName(player) {
  return `${player?.name || 'Unknown player'} (${player?.position || 'FLEX'} - ${player?.rosterSlot || 'bench'})`;
}

function renderTransactionPreview() {
  if (!previewPlayer || !directoryModel || !previewCard) return;
  const roster = getRoster();
  const rules = rosterRules();
  const needsDrop = directoryModel.requiresDrop(roster, rules);
  const dropId = needsDrop ? String(previewDropSelect?.value || '') : '';
  const preview = directoryModel.buildRosterPreview(previewPlayer, roster, rules, dropId);

  if (previewPlayerEl) {
    previewPlayerEl.innerHTML = `<strong>Add ${safeText(previewPlayer.name, 'Unknown player')}</strong><span class="muted">${safeText(previewPlayer.team, 'Team TBD')} / ${safeText(previewPlayer.position, '')}${previewPlayer.opponent ? ` / vs ${safeText(previewPlayer.opponent)}` : ''}</span>`;
  }
  if (previewRosterCount) previewRosterCount.textContent = `${preview.rosterCountBefore} / ${preview.rosterLimit}`;
  if (previewRosterAfter) previewRosterAfter.textContent = preview.valid && !(needsDrop && previewWeekLocked)
    ? `${preview.rosterCountAfter} / ${preview.rosterLimit}`
    : 'Needs valid drop';
  if (previewDestination) previewDestination.textContent = preview.destination ? preview.destination.toUpperCase() : '-';
  if (previewConfirm) {
    previewConfirm.textContent = needsDrop ? 'Confirm add/drop' : 'Confirm add';
    previewConfirm.disabled = previewBusy || !preview.valid || (needsDrop && previewWeekLocked);
  }
  if (previewCancel) previewCancel.disabled = previewBusy;
  if (previewMessage) {
    previewMessage.textContent = needsDrop && previewWeekLocked
      ? 'Roster drops are locked for the active fantasy week.'
      : needsDrop
        ? (dropId
            ? `This will add ${previewPlayer.name} and drop ${preview.drop?.name || 'the selected player'} in one server transaction.`
            : 'Your roster is full. Choose one eligible, unlocked player to drop.')
        : `This will add ${previewPlayer.name} without dropping a player.`;
  }
  if (previewRosterEl) {
    previewRosterEl.innerHTML = preview.resultingRoster.map((player) =>
      `<span class="pill pill--muted">${safeText(player.rosterSlot || 'bench').toUpperCase()}: ${safeText(player.name || player.id || 'Player')}</span>`
    ).join('');
  }
}

async function openTransactionPreview(player) {
  if (!directoryAvailable || !serverLeagueSelected() || String(player.availability || '') !== 'available') return;
  previewPlayer = player;
  previewBusy = true;
  if (previewCard) previewCard.hidden = false;
  if (previewPlayerEl) previewPlayerEl.textContent = `Loading ${player.name} roster impact...`;
  if (previewConfirm) previewConfirm.disabled = true;
  previewCard?.scrollIntoView?.({ behavior: 'smooth', block: 'nearest' });

  try {
    const [, lockState] = await Promise.all([syncRosterForDirectory(), fetchRosterLocks()]);
    if (!previewPlayer || previewPlayer.id !== player.id) return;
    previewLocks = Array.isArray(lockState?.players) ? lockState.players : [];
    previewWeekLocked = lockState?.weekLocked === true;
    const roster = getRoster();
    const rules = rosterRules();
    const needsDrop = directoryModel.requiresDrop(roster, rules);
    const candidates = previewWeekLocked
      ? []
      : directoryModel.eligibleDropCandidates(player, roster, rules, previewLocks);
    if (previewDropField) previewDropField.hidden = !needsDrop;
    if (previewDropSelect) {
      previewDropSelect.innerHTML = needsDrop
        ? `<option value="">Choose player to drop</option>${candidates.map((candidate) => `<option value="${safeText(candidate.id || candidate.playerId)}">${safeText(dropPlayerName(candidate))}</option>`).join('')}`
        : '';
      previewDropSelect.disabled = previewBusy || previewWeekLocked;
    }
  } catch (error) {
    window.CFF_UI?.notify(mutationErrorMessage(error, 'Could not load the latest roster impact.'), 'error');
    closeTransactionPreview();
    return;
  } finally {
    previewBusy = false;
    if (previewDropSelect) previewDropSelect.disabled = previewWeekLocked;
  }
  renderTransactionPreview();
}

previewDropSelect?.addEventListener('change', renderTransactionPreview);
previewCancel?.addEventListener('click', closeTransactionPreview);
previewConfirm?.addEventListener('click', async () => {
  if (!previewPlayer || previewBusy || !directoryModel) return;
  const roster = getRoster();
  const rules = rosterRules();
  const needsDrop = directoryModel.requiresDrop(roster, rules);
  const dropId = needsDrop ? String(previewDropSelect?.value || '') : '';
  const preview = directoryModel.buildRosterPreview(previewPlayer, roster, rules, dropId);
  if (!preview.valid || (needsDrop && previewWeekLocked)) {
    renderTransactionPreview();
    return;
  }

  previewBusy = true;
  renderTransactionPreview();
  try {
    await addFreeAgentApi(previewPlayer, dropId);
    const addedName = previewPlayer.name || 'Player';
    const droppedName = preview.drop?.name || '';
    window.CFF_UI?.notify(
      droppedName ? `Added ${addedName} and dropped ${droppedName}.` : `Added ${addedName}.`,
      'success'
    );
    closeTransactionPreview();
    await loadPlayerPool();
    if (typeof window.refreshLeagueDashboard === 'function') {
      void window.refreshLeagueDashboard({ allowCached: false }).catch(() => {});
    }
  } catch (error) {
    window.CFF_UI?.notify(
      error?.userMessage || mutationErrorMessage(error, 'The add/drop did not complete. No partial roster change was kept.'),
      'error'
    );
    closeTransactionPreview();
    try { await syncRosterForDirectory(); } catch {}
    await loadPlayerPool();
  } finally {
    previewBusy = false;
  }
});

function renderQueue() {
  const queue = getQueue();
  if (queueCount) queueCount.textContent = String(queue.length);
  if (!queueList) return;
  if (!queue.length) {
    queueList.innerHTML = `
      <div class="row">
        <div>
          <strong>No queued players yet</strong>
          <div class="muted">Use Queue in the directory to build a ranked draft shortlist.</div>
        </div>
      </div>
    `;
    return;
  }
  queueList.innerHTML = queue.map((player, index) => `
    <div class="row">
      <div>
        <strong>${index + 1}. ${safeText(player.name, 'Unknown player')}</strong>
        <div class="muted">${safeText(player.team, 'Team TBD')} ${safeText(player.position, 'FLEX')} / Rank ${safeNumber(player.rank, 99)}</div>
      </div>
      <button class="button button--ghost" data-remove-index="${index}" type="button">Remove</button>
    </div>
  `).join('');
  queueList.querySelectorAll('[data-remove-index]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = getQueue()[Number(button.dataset.removeIndex)];
      if (!player) return;
      const nextQueue = getQueue().filter((item) => item.id !== player.id);
      button.disabled = true;
      try {
        await saveDraftQueueApi(nextQueue);
        setQueue(nextQueue);
        window.CFF_UI?.notify(`${player.name} removed from your queue.`, 'info');
      } catch (error) {
        button.disabled = false;
        window.CFF_UI?.notify(mutationErrorMessage(error, 'Could not update draft queue. No local changes were made.'), 'error');
        return;
      }
      renderQueue();
      renderSearchResults(lastResults);
    });
  });
}

async function refreshLeagueDirectoryContext() {
  closeTransactionPreview();
  activeLeague = getLeagueState?.() || null;
  renderLeagueContext();
  directoryAvailable = false;
  if (serverLeagueSelected()) {
    try { await syncRosterForDirectory(); } catch {}
  }
  currentOffset = 0;
  await loadPlayerPool();
}

async function initPlayersPage() {
  await validateAuthSession();
  refreshAuthState();
  try {
    await syncLeaguesFromApi();
    await syncDraftFromApi();
  } catch {
    // Public browsing and the last confirmed draft queue remain usable.
  }
  activeLeague = getLeagueState?.() || null;
  renderLeagueContext();
  if (serverLeagueSelected()) {
    try { await syncRosterForDirectory(); } catch {}
  }
  renderQueue();
  await Promise.all([loadPlayerMeta(), loadPlayerPool()]);
}

initPlayersPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_QUEUE_KEY].includes(event.key)) {
    refreshAuthState();
    renderQueue();
  }
  if ([CFF_AUTH_KEY, CFF_LEAGUES_KEY, CFF_LEAGUE_KEY].includes(event.key)) {
    void refreshLeagueDirectoryContext();
  }
});
window.addEventListener('cff:active-league-changed', () => { void refreshLeagueDirectoryContext(); });
window.addEventListener('cff:roster-transaction', () => {
  if (!previewBusy) void loadPlayerPool();
});
