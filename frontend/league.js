const navSignInBtn = document.getElementById('nav-sign-in');
const navLogoutBtn = document.getElementById('nav-logout');
const emptyState = document.getElementById('league-empty');
const details = document.getElementById('league-details');
const leagueName = document.getElementById('league-name');
const leagueTeams = document.getElementById('league-teams');
const leagueScoring = document.getElementById('league-scoring');
const leagueDraft = document.getElementById('league-draft');
const leagueId = document.getElementById('league-id');
const leagueNotes = document.getElementById('league-notes');
const clearLeagueBtn = document.getElementById('clear-league');

let authState = null;
let leagueState = null;

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

function loadStoredLeague() {
  try {
    const raw = localStorage.getItem('cff_league');
    if (raw) {
      leagueState = JSON.parse(raw);
    }
  } catch {
    leagueState = null;
  }
}

function updateNav() {
  if (navSignInBtn) {
    if (authState) {
      navSignInBtn.textContent = authState.email || 'Account';
      navSignInBtn.href = 'index.html';
    } else {
      navSignInBtn.textContent = 'Sign in';
      navSignInBtn.href = 'signin.html';
    }
  }
  if (navLogoutBtn) {
    navLogoutBtn.hidden = !authState;
  }
}

function renderLeague() {
  if (!leagueState) {
    if (emptyState) emptyState.hidden = false;
    if (details) details.hidden = true;
    if (clearLeagueBtn) clearLeagueBtn.hidden = true;
    return;
  }

  if (emptyState) emptyState.hidden = true;
  if (details) details.hidden = false;
  if (clearLeagueBtn) clearLeagueBtn.hidden = false;

  const scoring = leagueState.scoringLabel || leagueState.scoring || 'ppr';
  const draft = leagueState.draftTypeLabel || leagueState.draftType || 'snake';

  leagueName.textContent = leagueState.name || 'League';
  leagueTeams.textContent = leagueState.teams ? `${leagueState.teams} teams` : 'Teams TBD';
  leagueScoring.textContent = scoring;
  leagueDraft.textContent = draft;
  leagueId.textContent = leagueState.id ? `ID: ${leagueState.id}` : 'ID not assigned';
  leagueNotes.textContent = leagueState.notes || 'No notes yet.';
}

function clearLeague() {
  leagueState = null;
  localStorage.removeItem('cff_league');
  renderLeague();
}

navLogoutBtn?.addEventListener('click', () => {
  localStorage.removeItem('cff_auth');
  localStorage.removeItem('cff_league');
  authState = null;
  leagueState = null;
  updateNav();
  renderLeague();
});

clearLeagueBtn?.addEventListener('click', () => {
  clearLeague();
});

loadStoredAuth();
loadStoredLeague();
updateNav();
renderLeague();
