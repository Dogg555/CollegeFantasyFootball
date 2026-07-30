const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO !== false;

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
const pendingInvite = new URLSearchParams(window.location.search).get('invite');

function setStatus(el, message, isError = false) {
  if (!el) return;
  el.textContent = message;
  el.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function saveAuth(email, token) {
  storedAuth = { email, token };
  setAuthState(storedAuth);
  updateAuthUi();
}

function loadStoredAuth() {
  storedAuth = getAuthState();
}

function clearAuth() {
  storedAuth = null;
  clearSessionState();
  updateAuthUi();
}

function updateAuthUi() {
  updateSharedNav('');
  if (authNote) {
    authNote.textContent = storedAuth?.email
      ? `Signed in as ${storedAuth.email}.`
      : 'Not signed in yet.';
  }
  if (signOutBtn) {
    signOutBtn.hidden = !storedAuth;
  }
}

function createLocalSession(email) {
  return {
    email,
    token: `local-demo-${Date.now().toString(36)}`,
    message: 'Local demo session created'
  };
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
      setStatus(statusEl, data?.error || 'Request failed', true);
      return;
    }
    saveAuth(data.email || email, data.token);
    setStatus(statusEl, data.message || 'Success');
  } catch {
    if (!allowLocalDemo) {
      setStatus(statusEl, 'API unavailable. Local demo sessions are disabled for this deployment.', true);
      return;
    }
    const local = createLocalSession(email);
    saveAuth(local.email, local.token);
    setStatus(statusEl, local.message);
  }
  if (redirectTo) {
    window.location.href = pendingInvite ? `${redirectTo}?invite=${encodeURIComponent(pendingInvite)}` : redirectTo;
  }
}

function stripUrlParams() {
  if (window.location.search) {
    window.history.replaceState({}, document.title, window.location.pathname);
  }
}

signupForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  await submitAuthForm('/auth/signup', signupEmail.value.trim(), signupPassword.value, signupStatus, pendingInvite ? 'league.html' : 'index.html');
});

loginForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  await submitAuthForm('/auth/login', loginEmail.value.trim(), loginPassword.value, loginStatus, 'league.html');
});

signOutBtn?.addEventListener('click', () => {
  clearAuth();
  setStatus(loginStatus, 'Signed out');
});

async function initAuthPage() {
  loadStoredAuth();
  updateAuthUi();
  await validateAuthSession();
  loadStoredAuth();
  updateAuthUi();
  stripUrlParams();
}

initAuthPage();
