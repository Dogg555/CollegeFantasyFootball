const apiBase = '/api';

const signupForm = document.getElementById('signup-form');
const signupEmail = document.getElementById('signup-email');
const signupPassword = document.getElementById('signup-password');
const signupStatus = document.getElementById('signup-status');

const loginForm = document.getElementById('login-form');
const loginEmail = document.getElementById('login-email');
const loginPassword = document.getElementById('login-password');
const loginStatus = document.getElementById('login-status');

const signOutBtn = document.getElementById('signout-btn');
const authNote = document.getElementById('auth-note');
let storedAuth = null;

function setStatus(el, message, isError = false) {
  if (!el) return;
  el.textContent = message;
  el.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function saveAuth(email, token) {
  storedAuth = { email, token };
  localStorage.setItem('cff_auth', JSON.stringify(storedAuth));
  updateAuthUi();
}

function loadStoredAuth() {
  try {
    const raw = localStorage.getItem('cff_auth');
    if (raw) {
      storedAuth = JSON.parse(raw);
    }
  } catch {
    storedAuth = null;
  }
}

function clearAuth() {
  storedAuth = null;
  localStorage.removeItem('cff_auth');
  localStorage.removeItem('cff_league');
  updateAuthUi();
}

function updateAuthUi() {
  if (authNote) {
    if (storedAuth && storedAuth.email) {
      authNote.textContent = `Signed in as ${storedAuth.email}.`;
    } else {
      authNote.textContent = 'Not signed in yet.';
    }
  }
  if (signOutBtn) {
    signOutBtn.hidden = !storedAuth;
  }
}
async function submitAuthForm(path, email, password, statusEl, redirectTo) {
  setStatus(statusEl, 'Working...');
  try {
    const resp = await fetch(`${apiBase}${path}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email, password }),
    });
    const data = await resp.json();
    if (!resp.ok) {
      const message = data?.error || 'Request failed';
      setStatus(statusEl, message, true);
      return;
    }
    saveAuth(data.email || email, data.token);
    setStatus(statusEl, data.message || 'Success');
    if (redirectTo) {
      window.location.href = redirectTo;
    }
  } catch (err) {
    setStatus(statusEl, 'Unable to reach the server. Is it running?', true);
  }
}

function stripUrlParams() {
  if (window.location.search) {
    window.history.replaceState({}, document.title, window.location.pathname);
  }
}

signupForm?.addEventListener('submit', async (e) => {
  e.preventDefault();
  await submitAuthForm('/auth/signup', signupEmail.value.trim(), signupPassword.value, signupStatus);
});

signOutBtn?.addEventListener('click', () => {
  clearAuth();
  setStatus(loginStatus, 'Signed out');
});

loginForm?.addEventListener('submit', async (e) => {
  e.preventDefault();
  await submitAuthForm('/auth/login', loginEmail.value.trim(), loginPassword.value, loginStatus, 'league.html');
});


loadStoredAuth();
updateAuthUi();

stripUrlParams();
if (storedAuth && storedAuth.token) {
  window.location.href = 'league.html';
}

