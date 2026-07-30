const apiBase = window.CFF_API_BASE || '/api';

const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const positionFilter = document.getElementById('position-filter');
const searchResultsEl = document.getElementById('search-results');
const queueList = document.getElementById('queue-list');
const queueCount = document.getElementById('queue-count');
const ingestStatusPill = document.getElementById('ingest-status-pill');
const ingestCounts = document.getElementById('ingest-counts');
const ingestStatus = document.getElementById('ingest-status');
const ingestRuns = document.getElementById('ingest-runs');
const refreshIngestStatusBtn = document.getElementById('refresh-ingest-status');
const runIngestBtn = document.getElementById('run-ingest');

function refreshAuthState() {
  updateSharedNav('players');
}

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  refreshAuthState();
  renderQueue();
  renderIngestSignedOut();
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

function renderIngestSignedOut() {
  if (ingestStatusPill) ingestStatusPill.textContent = 'Sign in';
  if (ingestStatus) ingestStatus.textContent = 'Sign in to view ingestion status.';
  if (ingestRuns) ingestRuns.textContent = 'Sign in to view ingestion status.';
  if (runIngestBtn) runIngestBtn.disabled = true;
  if (refreshIngestStatusBtn) refreshIngestStatusBtn.disabled = true;
}

function renderIngestStatus(payload = {}) {
  if (ingestStatusPill) ingestStatusPill.textContent = payload.status || 'Unknown';
  if (ingestCounts) {
    const counts = payload.counts || {};
    ingestCounts.innerHTML = `
      <div>
        <div class="label">Players</div>
        <div class="value">${counts.players ?? '--'}</div>
      </div>
      <div>
        <div class="label">Teams</div>
        <div class="value">${counts.teams ?? '--'}</div>
      </div>
      <div>
        <div class="label">Stats</div>
        <div class="value">${counts.playerStats ?? '--'}</div>
      </div>
    `;
  }
  if (ingestStatus) {
    ingestStatus.textContent = payload.configured === false
      ? 'Postgres ingestion status is unavailable for this backend build.'
      : `Recent ingestion status: ${payload.status || 'unknown'}.`;
  }
  const runs = payload.runs || [];
  if (!ingestRuns) return;
  if (!runs.length) {
    ingestRuns.textContent = 'No ingestion runs recorded yet.';
    return;
  }
  ingestRuns.innerHTML = runs.map((run) => `
    <div class="row">
      <div>
        <strong>${run.resource || 'resource'} / ${run.status || 'unknown'}</strong>
        <div class="muted">${run.startedAt || 'Not started'}${run.finishedAt ? ` / Finished ${new Date(run.finishedAt).toLocaleString()}` : ''}</div>
        ${run.error ? `<div class="muted small">${run.error}</div>` : ''}
      </div>
      <span class="badge">${run.rowCount || 0} rows</span>
    </div>
  `).join('');
}

async function refreshIngestStatus() {
  if (!getAuthState()?.token) {
    renderIngestSignedOut();
    return;
  }
  if (refreshIngestStatusBtn) refreshIngestStatusBtn.disabled = true;
  if (runIngestBtn) runIngestBtn.disabled = false;
  if (ingestStatus) ingestStatus.textContent = 'Loading ingestion status...';
  try {
    renderIngestStatus(await apiRequest('/admin/ingest/cfbd/status'));
  } catch (error) {
    if (ingestStatusPill) ingestStatusPill.textContent = 'Unavailable';
    if (ingestStatus) ingestStatus.textContent = error.message || 'Could not load ingestion status.';
    if (ingestRuns) ingestRuns.textContent = 'Ingestion status endpoint is unavailable.';
  } finally {
    if (refreshIngestStatusBtn) refreshIngestStatusBtn.disabled = false;
  }
}

async function runCfbdIngest() {
  if (!getAuthState()?.token) {
    renderIngestSignedOut();
    return;
  }
  if (runIngestBtn) runIngestBtn.disabled = true;
  if (ingestStatus) ingestStatus.textContent = 'Running CFBD ingest...';
  try {
    const result = await apiRequest('/admin/ingest/cfbd', { method: 'POST' });
    if (ingestStatus) {
      ingestStatus.textContent = `Ingest ${result.status || 'complete'}: ${result.ingested || 0} inserted, ${result.updated || 0} updated, ${result.apiCalls || 0} API calls.`;
    }
    await refreshIngestStatus();
  } catch (error) {
    if (ingestStatus) ingestStatus.textContent = error.message || 'Could not run CFBD ingest.';
  } finally {
    if (runIngestBtn) runIngestBtn.disabled = false;
  }
}

refreshIngestStatusBtn?.addEventListener('click', refreshIngestStatus);
runIngestBtn?.addEventListener('click', runCfbdIngest);

async function initPlayersPage() {
  await validateAuthSession();
  refreshAuthState();
  try {
    await syncLeaguesFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep local player queue available when the API is offline.
  }
  renderQueue();
  renderSearchResults(samplePlayers.slice(0, 6));
  await refreshIngestStatus();
}

initPlayersPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_QUEUE_KEY].includes(event.key)) {
    refreshAuthState();
    renderQueue();
  }
});
