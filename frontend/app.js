const apiBase = window.CFF_API_BASE || '/api';

const modal = document.getElementById('league-modal');
const modalBackdrop = modal?.querySelector('.modal__backdrop');
const closeModalBtn = document.getElementById('close-modal');
const cancelModalBtn = document.getElementById('cancel-modal');
const form = document.getElementById('create-league-form');
const leagueNameInput = document.getElementById('league-name');
const leagueSizeInput = document.getElementById('league-size');
const leagueScoringInput = document.getElementById('league-scoring');
const draftTypeInput = document.getElementById('draft-type');
const draftDateInput = document.getElementById('draft-date');
const notesInput = document.getElementById('league-notes');
const inviteEmailsInput = document.getElementById('invite-emails');
const formStatus = document.getElementById('form-status');
const liveScoresEl = document.getElementById('live-scores');
const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const searchResultsEl = document.getElementById('search-results');
const leagueSummaryEl = document.getElementById('league-summary');
const viewLeagueLink = document.getElementById('view-league');
const accountHint = document.getElementById('account-hint');
const draftQueuePreview = document.getElementById('draft-queue-preview');

let authState = null;
let leagueState = null;

document.querySelectorAll('.js-open-league').forEach((btn) => {
  btn.addEventListener('click', () => openModal());
});

function loadStoredAuth() {
  authState = getAuthState();
}

function loadStoredLeague() {
  leagueState = getLeagueState();
}

function persistLeague() {
  if (!leagueState) return { ok: true };
  return saveLeagueForAccount(leagueState);
}

function clearAuth() {
  authState = null;
  leagueState = null;
  clearSessionState();
  updateAuthUi();
  loadLeagueSummary();
  renderDraftQueuePreview();
}

function updateAuthUi() {
  updateSharedNav('home');
  if (viewLeagueLink) {
    viewLeagueLink.hidden = !leagueState;
  }
}

async function refreshState() {
  loadStoredAuth();
  if (authState?.token) {
    await validateAuthSession();
    loadStoredAuth();
  }
  if (authState?.token) {
    try {
      await syncLeaguesFromApi();
    } catch {
      // Local cache remains the fallback when the API is offline.
    }
  }
  loadStoredLeague();
  updateAuthUi();
  loadLeagueSummary();
  renderDraftQueuePreview();
}

function openModal() {
  if (!modal) return;
  if (authState && !canCreateLeague()) {
    setFormStatus(`You already have ${MAX_LEAGUES_PER_ACCOUNT} leagues on this account.`, true);
    renderLeagueSummary();
    return;
  }
  modal.classList.add('is-open');
  modal.setAttribute('aria-hidden', 'false');
  leagueNameInput?.focus();
  setFormStatus('Draft type defaults to snake.');
}

function closeModal() {
  if (!modal || !form) return;
  modal.classList.remove('is-open');
  modal.setAttribute('aria-hidden', 'true');
  form.reset();
  draftTypeInput.value = 'snake';
  setActiveSegment('snake');
  setFormStatus('');
}

modalBackdrop?.addEventListener('click', closeModal);
closeModalBtn?.addEventListener('click', closeModal);
cancelModalBtn?.addEventListener('click', closeModal);
document.getElementById('nav-logout')?.addEventListener('click', clearAuth);

document.querySelectorAll('.segment').forEach((btn) => {
  btn.addEventListener('click', () => {
    const value = btn.dataset.value;
    draftTypeInput.value = value;
    setActiveSegment(value);
  });
});

function setActiveSegment(value) {
  document.querySelectorAll('.segment').forEach((btn) => {
    btn.classList.toggle('is-active', btn.dataset.value === value);
  });
}

function setFormStatus(message, isError = false) {
  if (!formStatus) return;
  formStatus.textContent = message;
  formStatus.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function authHeaders() {
  return authState?.token ? { Authorization: `Bearer ${authState.token}` } : {};
}

async function fetchLiveScores() {
  if (!liveScoresEl) return;
  liveScoresEl.textContent = 'Loading live scores...';
  try {
    const resp = await fetch(`${apiBase}/scores/live`);
    if (!resp.ok) throw new Error('Live scores unavailable');
    renderLiveScores(await resp.json());
  } catch {
    renderLiveScores(sampleScores);
  }
}

function renderLiveScores(scores = []) {
  if (!liveScoresEl) return;
  if (!scores.length) {
    liveScoresEl.textContent = 'No games in progress.';
    return;
  }
  liveScoresEl.innerHTML = scores.map((score) => `
    <div class="row">
      <div>
        <strong>${score.away} @ ${score.home}</strong>
        <div class="muted">Q${score.quarter} - ${score.clock || '00:00'}</div>
      </div>
      <div class="score">${score.awayScore} - ${score.homeScore}</div>
    </div>
  `).join('');
}

function renderDraftQueuePreview() {
  if (!draftQueuePreview) return;
  const queue = getQueue();
  if (!queue.length) {
    draftQueuePreview.innerHTML = `
      <div class="row">
        <div>
          <strong>No players queued</strong>
          <div class="muted">Search the player pool and queue targets for your draft board.</div>
        </div>
        <a class="button" href="players.html">Find players</a>
      </div>
    `;
    return;
  }
  draftQueuePreview.innerHTML = queue.slice(0, 4).map((player, index) => `
    <div class="row">
      <div>
        <strong>${index + 1}. ${player.name}</strong>
        <div class="muted">${player.team} ${player.position} / ${Number(player.projection).toFixed(1)} proj</div>
      </div>
      <span class="badge">Rank ${player.rank}</span>
    </div>
  `).join('');
}

if (searchForm && searchInput && searchResultsEl) {
  searchForm.addEventListener('submit', async (event) => {
    event.preventDefault();
    const term = searchInput.value.trim();
    if (!term) return;
    searchResultsEl.textContent = 'Searching...';
    try {
      const resp = await fetch(`${apiBase}/players?query=${encodeURIComponent(term)}`);
      if (!resp.ok) throw new Error('Search failed');
      renderSearchResults((await resp.json()).map(normalizePlayer));
    } catch {
      renderSearchResults(filterSamplePlayers(term));
    }
  });
}

function renderSearchResults(players = []) {
  if (!searchResultsEl) return;
  if (!players.length) {
    searchResultsEl.textContent = 'No players found.';
    return;
  }
  searchResultsEl.innerHTML = players.slice(0, 10).map((player) => `
    <div class="row">
      <div>
        <strong>${player.name}</strong> - ${player.team} (${player.position})
        <div class="muted">${player.conference || 'Conference TBD'} / ${player.class || 'Class TBD'} / ${Number(player.projection).toFixed(1)} proj</div>
      </div>
      <button class="button" data-player="${player.id}" type="button">Add to queue</button>
    </div>
  `).join('');
  searchResultsEl.querySelectorAll('[data-player]').forEach((button) => {
    button.addEventListener('click', () => {
      const player = players.find((item) => item.id === button.dataset.player);
      if (!player) return;
      addPlayerToQueue(player);
      button.textContent = 'Queued';
      renderDraftQueuePreview();
    });
  });
}

form?.addEventListener('submit', async (event) => {
  event.preventDefault();
  const payload = {
    name: leagueNameInput.value.trim() || 'New League',
    teams: parseInt(leagueSizeInput.value, 10),
    scoring: leagueScoringInput.value,
    scoringSettings: normalizeScoringSettings(leagueScoringInput.value),
    draftType: draftTypeInput.value,
    draftDate: draftDateInput.value,
    notes: notesInput.value.trim(),
    invitedEmails: parseEmailList(inviteEmailsInput.value),
    rosterRules: defaultRosterRules,
  };

  setFormStatus('Saving league...');

  try {
    const resp = await fetch(`${apiBase}/leagues`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...authHeaders() },
      body: JSON.stringify(payload),
    });
    if (!resp.ok) {
      if (resp.status === 401) throw new Error('You must sign in before creating a league.');
      throw new Error('Unable to create league');
    }
    leagueState = normalizeLeague(await resp.json());
  } catch (err) {
    if (!authState) {
      setFormStatus('Sign in before creating a league.', true);
      return;
    }
    leagueState = normalizeLeague({
      ...payload,
      id: `local-${Date.now().toString(36)}`,
      message: 'League saved locally'
    });
  }

  const saveResult = persistLeague();
  if (!saveResult.ok) {
    setFormStatus(saveResult.error, true);
    return;
  }
  leagueState = saveResult.league || leagueState;
  updateAuthUi();
  renderLeagueSummary();
  setFormStatus('League saved. Draft room and invites are ready.');
  setTimeout(closeModal, 500);
});

function loadLeagueSummary() {
  if (!leagueSummaryEl) return;
  if (leagueState) {
    renderLeagueSummary();
    return;
  }
  const message = authState
    ? `Signed in as ${authState.email || 'manager'}. Create a league to see it here.`
    : 'Sign in to create and view leagues.';
  if (accountHint) {
    accountHint.textContent = authState
      ? `Signed in as ${authState.email || 'manager'}.`
      : 'Go to the sign-in page to create an account or log in.';
  }
  leagueSummaryEl.innerHTML = `
    <div class="row">
      <div>
        <strong>No leagues yet</strong>
        <div class="muted">${message}</div>
      </div>
    </div>
  `;
}

function renderLeagueSummary() {
  if (!leagueSummaryEl) return;
  if (!leagueState) {
    loadLeagueSummary();
    return;
  }
  const leagues = getLeaguesForCurrentAccount();
  const leagueRows = leagues.length > 1 ? `
    <div class="row">
      <div>
        <strong>Leagues on this account</strong>
        <div class="muted small">${leagues.length} / ${MAX_LEAGUES_PER_ACCOUNT} used</div>
      </div>
      <a class="button" href="league.html">Switch league</a>
    </div>
  ` : '';

  leagueSummaryEl.innerHTML = `
    <div class="row">
      <div>
        <strong>${leagueState.name}</strong> - ${leagueState.teams} teams
        <div class="muted">
          ${leagueState.scoringLabel || leagueState.scoring} / ${leagueState.draftTypeLabel || leagueState.draftType} draft
        </div>
        <div class="muted small">${scoringSummary(leagueState.scoringSettings)}</div>
        <div class="muted small">${leagueState.draftDate ? `Draft: ${new Date(leagueState.draftDate).toLocaleString()}` : 'Draft date not set'}</div>
      </div>
      <div class="badge">ID: ${leagueState.id}</div>
    </div>
    <div class="row">
      <div>
        <div class="muted">Next step: send invites and schedule the draft.</div>
        <div class="muted small">${leagueState.invitedEmails?.length || 0} manager invites / ${leagueState.notes || 'No manager notes yet.'}</div>
      </div>
      <a class="button" href="league.html">View league</a>
    </div>
    ${leagueRows}
  `;
}

refreshState();
fetchLiveScores();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_LEAGUE_KEY, CFF_LEAGUES_KEY, CFF_QUEUE_KEY].includes(event.key)) {
    refreshState();
  }
});

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) refreshState();
});
