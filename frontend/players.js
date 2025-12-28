const apiBase = '/api';

const navSignInBtn = document.getElementById('nav-sign-in');
const navLogoutBtn = document.getElementById('nav-logout');
const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const searchResultsEl = document.getElementById('search-results');

let authState = null;

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

function clearAuth() {
  authState = null;
  localStorage.removeItem('cff_auth');
  localStorage.removeItem('cff_league');
  updateAuthUi();
}

function updateAuthUi() {
  if (navSignInBtn) {
    if (authState) {
      navSignInBtn.textContent = authState.email || 'Account';
      navSignInBtn.href = 'league.html';
    } else {
      navSignInBtn.textContent = 'Sign in';
      navSignInBtn.href = 'signin.html';
    }
  }
  if (navLogoutBtn) {
    navLogoutBtn.hidden = !authState;
  }
}

function refreshAuthState() {
  loadStoredAuth();
  updateAuthUi();
}

navLogoutBtn?.addEventListener('click', () => {
  clearAuth();
});

if (searchForm && searchInput && searchResultsEl) {
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
    searchResultsEl.innerHTML = players.slice(0, 20).map(p => `
      <div class="row">
        <div>
          <strong>${p.name}</strong> — ${p.team} (${p.position || 'Pos TBD'})
          <div class="muted">${p.conference || 'Conference TBD'} • ${p.class || 'Class TBD'}</div>
        </div>
        <button class="button" data-player="${p.id}">Add to queue</button>
      </div>
    `).join('');
  }
}

// Initial bootstrap
refreshAuthState();

window.addEventListener('storage', (event) => {
  if (event.key === 'cff_auth') {
    refreshAuthState();
  }
});

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) {
    refreshAuthState();
  }
});
