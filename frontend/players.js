const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO === true
  && ['localhost', '127.0.0.1', '::1'].includes(window.location.hostname);
const PAGE_SIZE = 50;

const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const positionFilter = document.getElementById('position-filter');
const searchResultsEl = document.getElementById('search-results');
const queueList = document.getElementById('queue-list');
const queueCount = document.getElementById('queue-count');
const playerDataStatus = document.getElementById('player-data-status');
const loadMorePlayers = document.getElementById('load-more-players');
const playerResultCount = document.getElementById('player-result-count');
const playerMetaCount = document.getElementById('player-meta-count');
const playerMetaTeams = document.getElementById('player-meta-teams');
const playerMetaSeason = document.getElementById('player-meta-season');
const playerMetaUpdated = document.getElementById('player-meta-updated');

let lastResults = [];
let currentOffset = 0;
let hasMore = false;
let loadingPlayers = false;

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

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  refreshAuthState();
  renderQueue();
});

async function fetchPlayers(term = '', position = '', offset = 0) {
  const params = new URLSearchParams({ limit: String(PAGE_SIZE), offset: String(offset) });
  if (term) params.set('query', term);
  if (position) params.set('position', position);
  const response = await fetch(`${apiBase}/players?${params.toString()}`, {
    headers: { Accept: 'application/json' },
  });
  if (!response.ok) throw new Error(`Player search failed with ${response.status}.`);
  return (await response.json()).map((player) => ({
    ...normalizePlayer(player),
    season: safeNumber(player.season, 0),
    updatedAt: player.updatedAt || '',
  }));
}

async function fetchPlayerMeta() {
  const response = await fetch(`${apiBase}/players/meta`, { headers: { Accept: 'application/json' } });
  if (!response.ok) throw new Error(`Player metadata failed with ${response.status}.`);
  return response.json();
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

async function loadPlayerMeta() {
  try {
    renderPlayerMeta(await fetchPlayerMeta());
  } catch {
    renderPlayerMeta({ status: 'unavailable' });
  }
}

async function loadPlayerPool({ append = false } = {}) {
  if (!searchResultsEl || loadingPlayers) return;
  loadingPlayers = true;
  const term = searchInput?.value.trim() || '';
  const position = positionFilter?.value || '';
  const offset = append ? currentOffset : 0;
  if (!append) searchResultsEl.textContent = term ? 'Searching current rosters...' : 'Loading current-season players...';
  if (loadMorePlayers) loadMorePlayers.disabled = true;

  try {
    const batch = await fetchPlayers(term, position, offset);
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
  } catch {
    if (allowLocalDemo && !append) {
      lastResults = applyPositionFilter(filterSamplePlayers(term), position);
      renderSearchResults(lastResults, true);
      if (playerDataStatus) playerDataStatus.textContent = 'Offline player preview';
      if (playerResultCount) playerResultCount.textContent = `${lastResults.length} preview players shown`;
    } else if (!append) {
      lastResults = [];
      searchResultsEl.textContent = 'The current player database is temporarily unavailable.';
      if (playerDataStatus) playerDataStatus.textContent = 'Player sync unavailable';
      if (playerResultCount) playerResultCount.textContent = '0 players shown';
    }
    if (loadMorePlayers) loadMorePlayers.hidden = true;
  } finally {
    loadingPlayers = false;
  }
}

searchForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  currentOffset = 0;
  await loadPlayerPool();
});

positionFilter?.addEventListener('change', async () => {
  currentOffset = 0;
  await loadPlayerPool();
});

loadMorePlayers?.addEventListener('click', () => loadPlayerPool({ append: true }));

function applyPositionFilter(players, position) {
  if (!position) return players;
  return players.filter((player) => player.position === position);
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
    const queued = queuedIds.has(player.id);
    const seasonLabel = player.season ? ` / ${safeNumber(player.season)}` : '';
    return `
      <div class="row">
        <div>
          <strong>${safeText(player.name, 'Unknown player')}</strong> - ${safeText(player.team, 'Team TBD')} (${safeText(player.position, 'FLEX')})
          <div class="muted">${safeText(player.conference, 'Conference TBD')} / ${safeText(player.class, 'Class TBD')}${seasonLabel}</div>
        </div>
        <button class="button" data-player-index="${index}" type="button" ${queued ? 'disabled' : ''}>${queued ? 'Queued' : 'Add to queue'}</button>
      </div>
    `;
  }).join('');

  searchResultsEl.querySelectorAll('[data-player-index]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = players[Number(button.dataset.playerIndex)];
      if (!player) return;
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
    });
  });
}

function renderQueue() {
  const queue = getQueue();
  if (queueCount) queueCount.textContent = String(queue.length);
  if (!queueList) return;
  if (!queue.length) {
    queueList.innerHTML = `
      <div class="row">
        <div>
          <strong>No queued players yet</strong>
          <div class="muted">Search above to build a ranked draft shortlist.</div>
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

async function initPlayersPage() {
  await validateAuthSession();
  refreshAuthState();
  try {
    await syncLeaguesFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep the local player queue available when the API is offline.
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
});
