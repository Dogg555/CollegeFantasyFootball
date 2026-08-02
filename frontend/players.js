const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO !== false;

const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const positionFilter = document.getElementById('position-filter');
const searchResultsEl = document.getElementById('search-results');
const queueList = document.getElementById('queue-list');
const queueCount = document.getElementById('queue-count');
const playerDataStatus = document.getElementById('player-data-status');

let lastResults = [];

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

async function fetchPlayers(term = '', position = '') {
  const params = new URLSearchParams({ limit: '50' });
  // The API requires a query value. PostgreSQL's ILIKE wildcard returns the
  // complete active player pool when the user is browsing without search text.
  params.set('query', term || '%');
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

async function loadPlayerPool() {
  if (!searchResultsEl) return;
  const term = searchInput?.value.trim() || '';
  const position = positionFilter?.value || '';
  searchResultsEl.textContent = term ? 'Searching current rosters...' : 'Loading current-season players...';
  try {
    lastResults = await fetchPlayers(term, position);
    renderSearchResults(lastResults);
    const seasons = lastResults.map((player) => player.season).filter(Boolean);
    const season = seasons.length ? Math.max(...seasons) : null;
    if (playerDataStatus) {
      playerDataStatus.textContent = season
        ? `${season} active FBS rosters / refreshed by the weekly roster sync`
        : 'Active FBS rosters / refreshed by the weekly roster sync';
    }
  } catch (error) {
    if (allowLocalDemo) {
      lastResults = applyPositionFilter(filterSamplePlayers(term), position);
      renderSearchResults(lastResults, true);
      if (playerDataStatus) playerDataStatus.textContent = 'Offline sample pool';
      return;
    }
    lastResults = [];
    searchResultsEl.textContent = 'The current player database is temporarily unavailable.';
    if (playerDataStatus) playerDataStatus.textContent = 'Player sync unavailable';
  }
}

searchForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  await loadPlayerPool();
});

positionFilter?.addEventListener('change', loadPlayerPool);

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
    ? '<div class="row"><div><strong>Offline player pool</strong><div class="muted">Showing sample players until the current roster database is reachable.</div></div></div>'
    : '';
  searchResultsEl.innerHTML = notice + players.slice(0, 50).map((player, index) => {
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
      addPlayerToQueue(player);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // The local queue remains available while the API is offline.
      }
      button.textContent = 'Queued';
      button.disabled = true;
      window.CFF_UI?.notify(`${player.name} added to your draft queue.`, 'success');
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
      removeFromQueue(player.id);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // The local queue remains updated.
      }
      window.CFF_UI?.notify(`${player.name} removed from your queue.`, 'info');
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
  await loadPlayerPool();
}

initPlayersPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_QUEUE_KEY].includes(event.key)) {
    refreshAuthState();
    renderQueue();
  }
});
