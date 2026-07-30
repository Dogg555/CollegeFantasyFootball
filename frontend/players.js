const apiBase = '/api';

const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const positionFilter = document.getElementById('position-filter');
const searchResultsEl = document.getElementById('search-results');
const queueList = document.getElementById('queue-list');
const queueCount = document.getElementById('queue-count');

function refreshAuthState() {
  updateSharedNav('players');
}

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  refreshAuthState();
  renderQueue();
});

searchForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  const term = searchInput.value.trim();
  const position = positionFilter.value;
  if (!term) return;
  searchResultsEl.textContent = 'Searching...';
  try {
    const params = new URLSearchParams({ query: term });
    if (position) params.set('position', position);
    const resp = await fetch(`${apiBase}/players?${params.toString()}`);
    if (!resp.ok) throw new Error('Search failed');
    renderSearchResults(applyPositionFilter((await resp.json()).map(normalizePlayer), position));
  } catch {
    renderSearchResults(applyPositionFilter(filterSamplePlayers(term), position));
  }
});

positionFilter?.addEventListener('change', () => {
  const term = searchInput.value.trim();
  renderSearchResults(applyPositionFilter(filterSamplePlayers(term), positionFilter.value));
});

function applyPositionFilter(players, position) {
  if (!position) return players;
  return players.filter((player) => player.position === position);
}

function renderSearchResults(players = []) {
  if (!searchResultsEl) return;
  if (!players.length) {
    searchResultsEl.textContent = 'No players found.';
    return;
  }
  searchResultsEl.innerHTML = players.slice(0, 20).map((player) => `
    <div class="row">
      <div>
        <strong>${player.name}</strong> - ${player.team} (${player.position || 'Pos TBD'})
        <div class="muted">${player.conference || 'Conference TBD'} / ${player.class || 'Class TBD'} / ${Number(player.projection).toFixed(1)} proj</div>
      </div>
      <button class="button" data-player="${player.id}" type="button">Add to queue</button>
    </div>
  `).join('');

  searchResultsEl.querySelectorAll('[data-player]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = players.find((item) => item.id === button.dataset.player);
      if (!player) return;
      addPlayerToQueue(player);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // Local queue remains updated.
      }
      button.textContent = 'Queued';
      renderQueue();
    });
  });
}

function renderQueue() {
  const queue = getQueue();
  if (queueCount) queueCount.textContent = String(queue.length);
  if (!queueList) return;
  if (!queue.length) {
    queueList.textContent = 'No queued players yet.';
    return;
  }
  queueList.innerHTML = queue.map((player, index) => `
    <div class="row">
      <div>
        <strong>${index + 1}. ${player.name}</strong>
        <div class="muted">${player.team} ${player.position} / Rank ${player.rank}</div>
      </div>
      <button class="button button--ghost" data-remove="${player.id}" type="button">Remove</button>
    </div>
  `).join('');
  queueList.querySelectorAll('[data-remove]').forEach((button) => {
    button.addEventListener('click', async () => {
      removeFromQueue(button.dataset.remove);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // Local queue remains updated.
      }
      renderQueue();
    });
  });
}

async function initPlayersPage() {
  refreshAuthState();
  try {
    await syncLeaguesFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep local player queue available when the API is offline.
  }
  renderQueue();
  renderSearchResults(samplePlayers.slice(0, 6));
}

initPlayersPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_QUEUE_KEY].includes(event.key)) {
    refreshAuthState();
    renderQueue();
  }
});
