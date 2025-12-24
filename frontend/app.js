const apiBase = '/api';

const cta = document.getElementById('cta');
cta.addEventListener('click', () => {
  alert('League creation flow coming soon.');
});

const liveScoresEl = document.getElementById('live-scores');
const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const searchResultsEl = document.getElementById('search-results');
const leagueSummaryEl = document.getElementById('league-summary');

async function fetchLiveScores() {
  liveScoresEl.textContent = 'Loading live scores...';
  try {
    const resp = await fetch(`${apiBase}/scores/live`);
    if (!resp.ok) throw new Error('Live scores unavailable');
    const data = await resp.json();
    renderLiveScores(data);
  } catch (err) {
    liveScoresEl.textContent = 'Live scores will appear here during game time.';
  }
}

function renderLiveScores(scores = []) {
  if (!scores.length) {
    liveScoresEl.textContent = 'No games in progress.';
    return;
  }
  liveScoresEl.innerHTML = scores.map(s => `
    <div class="row">
      <div>
        <strong>${s.away} @ ${s.home}</strong>
        <div class="muted">Q${s.quarter} - ${s.clock || '00:00'}</div>
      </div>
      <div class="score">${s.awayScore} - ${s.homeScore}</div>
    </div>
  `).join('');
}

searchForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const term = searchInput.value.trim();
  if (!term) return;
  searchResultsEl.textContent = 'Searching...';
  try {
    const resp = await fetch(`${apiBase}/players?query=${encodeURIComponent(term)}`);
    if (!resp.ok) throw new Error('Search failed');
    const data = await resp.json();
    renderSearchResults(data);
  } catch (err) {
    searchResultsEl.textContent = 'Unable to fetch players right now.';
  }
});

function renderSearchResults(players = []) {
  if (!players.length) {
    searchResultsEl.textContent = 'No players found.';
    return;
  }
  searchResultsEl.innerHTML = players.slice(0, 10).map(p => `
    <div class="row">
      <div>
        <strong>${p.name}</strong> — ${p.team} (${p.position})
        <div class="muted">${p.conference || 'Conference TBD'} • ${p.class || 'Class TBD'}</div>
      </div>
      <button class="button" data-player="${p.id}">Add to queue</button>
    </div>
  `).join('');
}

function loadLeagueSummary() {
  leagueSummaryEl.innerHTML = `
    <div class="row">
      <div>
        <strong>League features</strong>
        <div class="muted">Create leagues, customize scoring, draft players, and manage lineups.</div>
      </div>
      <a class="button" href="#">Sign in</a>
    </div>
  `;
}

// Initial bootstrap
fetchLiveScores();
loadLeagueSummary();
