const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO === true
  && ['localhost', '127.0.0.1', '::1'].includes(window.location.hostname);

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

document.querySelectorAll('.js-open-league').forEach((button) => {
  button.addEventListener('click', () => openModal());
});

function safeText(value, fallback = '') {
  return escapeHtml(value ?? fallback);
}

function safeNumber(value, fallback = 0) {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : fallback;
}

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
  if (viewLeagueLink) viewLeagueLink.hidden = !leagueState;
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
      // The account cache remains available while the service is offline.
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
    window.CFF_UI?.notify(`League limit reached: ${MAX_LEAGUES_PER_ACCOUNT} per account.`, 'error');
    return;
  }
  modal.classList.add('is-open');
  modal.setAttribute('aria-hidden', 'false');
  document.body.classList.add('modal-open');
  leagueNameInput?.focus();
  setFormStatus(authState
    ? 'Draft type defaults to snake. You can change every setting later.'
    : 'Sign in before saving a league.');
}

function closeModal() {
  if (!modal || !form) return;
  modal.classList.remove('is-open');
  modal.setAttribute('aria-hidden', 'true');
  document.body.classList.remove('modal-open');
  form.reset();
  if (draftTypeInput) draftTypeInput.value = 'snake';
  setActiveSegment('snake');
  setFormStatus('');
}

modalBackdrop?.addEventListener('click', closeModal);
closeModalBtn?.addEventListener('click', closeModal);
cancelModalBtn?.addEventListener('click', closeModal);
document.getElementById('nav-logout')?.addEventListener('click', clearAuth);

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && modal?.classList.contains('is-open')) closeModal();
});

document.querySelectorAll('.segment').forEach((button) => {
  button.addEventListener('click', () => {
    const value = button.dataset.value;
    if (draftTypeInput) draftTypeInput.value = value;
    setActiveSegment(value);
  });
});

function setActiveSegment(value) {
  document.querySelectorAll('.segment').forEach((button) => {
    const active = button.dataset.value === value;
    button.classList.toggle('is-active', active);
    button.setAttribute('aria-pressed', String(active));
  });
}

function setFormStatus(message, isError = false) {
  if (!formStatus) return;
  formStatus.textContent = message;
  formStatus.classList.toggle('is-error', isError);
  formStatus.style.color = isError ? 'var(--danger)' : 'var(--muted)';
}

async function fetchLiveScores() {
  if (!liveScoresEl) return;
  liveScoresEl.textContent = 'Loading live scores...';
  try {
    const response = await fetch(`${apiBase}/scores/live`);
    if (!response.ok) throw new Error('Live scores are temporarily unavailable.');
    renderLiveScores(await response.json());
  } catch {
    renderLiveScores(sampleScores, true);
  }
}

function renderLiveScores(scores = [], fallback = false) {
  if (!liveScoresEl) return;
  if (!scores.length) {
    liveScoresEl.textContent = 'No games are currently in progress.';
    return;
  }
  const notice = fallback
    ? '<div class="row"><div><strong>Offline scoreboard preview</strong><div class="muted">Live data is unavailable, so preview games are shown.</div></div></div>'
    : '';
  liveScoresEl.innerHTML = notice + scores.map((score) => `
    <div class="row">
      <div>
        <strong>${safeText(score.away, 'Away')} @ ${safeText(score.home, 'Home')}</strong>
        <div class="muted">Q${safeNumber(score.quarter, 0)} - ${safeText(score.clock, '00:00')}</div>
      </div>
      <div class="score">${safeNumber(score.awayScore)} - ${safeNumber(score.homeScore)}</div>
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
        <strong>${index + 1}. ${safeText(player.name, 'Unknown player')}</strong>
        <div class="muted">${safeText(player.team, 'Team TBD')} ${safeText(player.position, 'FLEX')} / ${safeNumber(player.projection, 0).toFixed(1)} proj</div>
      </div>
      <span class="badge">Rank ${safeNumber(player.rank, 99)}</span>
    </div>
  `).join('');
}

if (searchForm && searchInput && searchResultsEl) {
  searchForm.addEventListener('submit', async (event) => {
    event.preventDefault();
    const term = searchInput.value.trim();
    if (!term) {
      searchResultsEl.textContent = 'Enter a player, school, position, or conference.';
      searchInput.focus();
      return;
    }
    searchResultsEl.textContent = 'Searching...';
    try {
      const response = await fetch(`${apiBase}/players?query=${encodeURIComponent(term)}`);
      if (!response.ok) throw new Error('Player search failed.');
      renderSearchResults((await response.json()).map(normalizePlayer));
    } catch {
      renderSearchResults(filterSamplePlayers(term), true);
    }
  });
}

function renderSearchResults(players = [], fallback = false) {
  if (!searchResultsEl) return;
  if (!players.length) {
    searchResultsEl.textContent = 'No players matched that search.';
    return;
  }
  const queuedIds = new Set(getQueue().map((player) => player.id));
  const notice = fallback
    ? '<div class="row"><div><strong>Offline player preview</strong><div class="muted">Showing preview players until the API is reachable.</div></div></div>'
    : '';
  searchResultsEl.innerHTML = notice + players.slice(0, 10).map((player, index) => {
    const queued = queuedIds.has(player.id);
    return `
      <div class="row">
        <div>
          <strong>${safeText(player.name, 'Unknown player')}</strong> - ${safeText(player.team, 'Team TBD')} (${safeText(player.position, 'FLEX')})
          <div class="muted">${safeText(player.conference, 'Conference TBD')} / ${safeText(player.class, 'Class TBD')} / ${safeNumber(player.projection, 0).toFixed(1)} proj</div>
        </div>
        <button class="button" data-player-index="${index}" type="button" ${queued ? 'disabled' : ''}>${queued ? 'Queued' : 'Add to queue'}</button>
      </div>
    `;
  }).join('');
  searchResultsEl.querySelectorAll('[data-player-index]').forEach((button) => {
    button.addEventListener('click', () => {
      const player = players[Number(button.dataset.playerIndex)];
      if (!player) return;
      addPlayerToQueue(player);
      button.textContent = 'Queued';
      button.disabled = true;
      window.CFF_UI?.notify(`${player.name} added to your draft queue.`, 'success');
      renderDraftQueuePreview();
    });
  });
}

form?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!authState) {
    setFormStatus('Sign in before creating a league.', true);
    window.CFF_UI?.notify('Sign in before creating a league.', 'error');
    return;
  }

  const invitedEmails = parseEmailList(inviteEmailsInput.value)
    .map((email) => email.toLowerCase());
  const payload = {
    name: leagueNameInput.value.trim() || 'New League',
    teams: parseInt(leagueSizeInput.value, 10),
    scoring: leagueScoringInput.value,
    scoringSettings: normalizeScoringSettings(leagueScoringInput.value),
    draftType: draftTypeInput.value,
    draftDate: draftDateInput.value,
    notes: notesInput.value.trim(),
    invitedEmails,
    rosterRules: defaultRosterRules,
  };

  if (payload.draftDate && new Date(payload.draftDate).getTime() <= Date.now()) {
    setFormStatus('Choose a draft date in the future.', true);
    draftDateInput.focus();
    return;
  }
  if (payload.draftDate && !isTopOfHourDraftDate(payload.draftDate)) {
    setFormStatus('Draft time must be scheduled at the top of an hour.', true);
    draftDateInput.focus();
    return;
  }

  setFormStatus('Saving league...');

  try {
    leagueState = normalizeLeague(await apiRequest('/leagues', {
      method: 'POST',
      body: JSON.stringify(payload)
    }));
  } catch (error) {
    if (error.status) {
      setFormStatus(mutationErrorMessage(error, 'The server rejected these league settings. No local league was created.'), true);
      return;
    }
    if (!allowLocalDemo || !String(authState?.token || '').startsWith('local-demo-')) {
      setFormStatus('The API is unavailable. Try again when the connection is restored.', true);
      return;
    }
    leagueState = normalizeLeague({
      ...payload,
      id: `local-${Date.now().toString(36)}`,
      message: 'League saved locally'
    });
    window.CFF_UI?.notify('The API is offline, so this league was saved locally for now.', 'info', 6000);
  }

  const saveResult = persistLeague();
  if (!saveResult.ok) {
    setFormStatus(saveResult.error, true);
    return;
  }
  leagueState = saveResult.league || leagueState;
  updateAuthUi();
  renderLeagueSummary();
  setFormStatus('League saved. Draft room and manager settings are ready.');
  window.CFF_UI?.notify(`${leagueState.name} is ready.`, 'success');
  window.setTimeout(closeModal, 650);
});

function loadLeagueSummary() {
  if (!leagueSummaryEl) return;
  if (leagueState) {
    renderLeagueSummary();
    return;
  }
  const message = authState
    ? `Signed in as ${safeText(authState.email, 'manager')}. Create a league to see it here.`
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

  const draftDate = leagueState.draftDate
    ? `Draft: ${safeText(new Date(leagueState.draftDate).toLocaleString())}`
    : 'Draft date not set';
  const notes = leagueState.notes ? safeText(leagueState.notes) : 'No manager notes yet.';
  const scoring = safeText(leagueState.scoringLabel || leagueState.scoring, 'PPR');
  const draftType = safeText(leagueState.draftTypeLabel || leagueState.draftType, 'Snake');

  leagueSummaryEl.innerHTML = `
    <div class="row">
      <div>
        <strong>${safeText(leagueState.name, 'League')}</strong> - ${safeNumber(leagueState.teams, 10)} teams
        <div class="muted">${scoring} / ${draftType} draft</div>
        <div class="muted small">${safeText(scoringSummary(leagueState.scoringSettings))}</div>
        <div class="muted small">${draftDate}</div>
      </div>
      <div class="badge">ID: ${safeText(leagueState.id, 'pending')}</div>
    </div>
    <div class="row">
      <div>
        <div class="muted">Next step: confirm members and open the draft lobby.</div>
        <div class="muted small">${leagueState.invitedEmails?.length || 0} manager invites / ${notes}</div>
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
