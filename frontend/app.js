const apiBase = '/api';

const modal = document.getElementById('league-modal');
const modalBackdrop = modal.querySelector('.modal__backdrop');
const closeModalBtn = document.getElementById('close-modal');
const cancelModalBtn = document.getElementById('cancel-modal');
const form = document.getElementById('create-league-form');
const navSignInBtn = document.getElementById('nav-sign-in');
const signInForm = document.getElementById('sign-in-form');
const signInEmailInput = document.getElementById('sign-in-email');
const signInTokenInput = document.getElementById('sign-in-token');
const signInStatus = document.getElementById('sign-in-status');
const prefillTokenBtn = document.getElementById('prefill-token');
const leagueNameInput = document.getElementById('league-name');
const leagueSizeInput = document.getElementById('league-size');
const leagueScoringInput = document.getElementById('league-scoring');
const draftTypeInput = document.getElementById('draft-type');
const notesInput = document.getElementById('league-notes');
const formStatus = document.getElementById('form-status');

const liveScoresEl = document.getElementById('live-scores');
const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const searchResultsEl = document.getElementById('search-results');
const leagueSummaryEl = document.getElementById('league-summary');
const accountHint = document.getElementById('account-hint');
let authState = null;
let leagueState = null;

document.querySelectorAll('.js-open-league').forEach(btn => {
  btn.addEventListener('click', () => openModal());
});

function loadStoredAuth() {
  try {
    const raw = localStorage.getItem('cff_auth');
    if (raw) {
      authState = JSON.parse(raw);
    }
  } catch {
    authState = null;
  }
}

function persistAuth() {
  if (!authState) return;
  localStorage.setItem('cff_auth', JSON.stringify(authState));
}

function openModal() {
  modal.classList.add('is-open');
  modal.setAttribute('aria-hidden', 'false');
  leagueNameInput.focus();
  setFormStatus('Draft type defaults to snake (like NFL Fantasy).');
}

function closeModal() {
  modal.classList.remove('is-open');
  modal.setAttribute('aria-hidden', 'true');
  form.reset();
  draftTypeInput.value = 'snake';
  setActiveSegment('snake');
  setFormStatus('');
}

modalBackdrop.addEventListener('click', closeModal);
closeModalBtn.addEventListener('click', closeModal);
cancelModalBtn.addEventListener('click', closeModal);

document.querySelectorAll('.segment').forEach(btn => {
  btn.addEventListener('click', () => {
    const value = btn.dataset.value;
    draftTypeInput.value = value;
    setActiveSegment(value);
  });
});

function setActiveSegment(value) {
  document.querySelectorAll('.segment').forEach(btn => {
    btn.classList.toggle('is-active', btn.dataset.value === value);
  });
}

function setFormStatus(message, isError = false) {
  formStatus.textContent = message;
  formStatus.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function authHeaders() {
  if (authState && authState.token) {
    return { Authorization: `Bearer ${authState.token}` };
  }
  return {};
}

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

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  const payload = {
    name: leagueNameInput.value.trim() || 'New League',
    teams: parseInt(leagueSizeInput.value, 10),
    scoring: leagueScoringInput.value,
    draftType: draftTypeInput.value,
    notes: notesInput.value.trim(),
  };

  setFormStatus('Saving league...');

  try {
    const resp = await fetch(`${apiBase}/leagues`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...authHeaders() },
      body: JSON.stringify(payload),
    });
    if (!resp.ok) {
      if (resp.status === 401) {
        throw new Error('You must sign in before creating a league.');
      }
      throw new Error('Unable to create league');
    }
    const data = await resp.json();
    leagueState = data;
    renderLeagueSummary();
    setFormStatus('League saved. Draft date and invites are up next.');
    setTimeout(closeModal, 500);
  } catch (err) {
    setFormStatus('Could not create the league right now. Try again soon.', true);
  }
});

function loadLeagueSummary() {
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
  if (!leagueState) {
    loadLeagueSummary();
    return;
  }

  leagueSummaryEl.innerHTML = `
    <div class="row">
      <div>
        <strong>${leagueState.name}</strong> — ${leagueState.teams} teams
        <div class="muted">
          ${leagueState.scoringLabel || leagueState.scoring} • ${leagueState.draftTypeLabel || leagueState.draftType} draft
        </div>
      </div>
      <div class="badge">ID: ${leagueState.id}</div>
    </div>
    <div class="row">
      <div>
        <div class="muted">Next step: send invites and schedule the draft.</div>
        <div class="muted small">${leagueState.notes || 'We’ll remind managers 1 hour before the draft.'}</div>
      </div>
      <button class="button">Manage league</button>
    </div>
  `;
}

// Initial bootstrap
loadStoredAuth();
fetchLiveScores();
loadLeagueSummary();
