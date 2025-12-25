const apiBase = '/api';

const signupForm = document.getElementById('signup-form');
const signupEmail = document.getElementById('signup-email');
const signupPassword = document.getElementById('signup-password');
const signupStatus = document.getElementById('signup-status');

const loginForm = document.getElementById('login-form');
const loginEmail = document.getElementById('login-email');
const loginPassword = document.getElementById('login-password');
const loginStatus = document.getElementById('login-status');

function setStatus(el, message, isError = false) {
  if (!el) return;
  el.textContent = message;
  el.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function saveAuth(email, token) {
  localStorage.setItem('cff_auth', JSON.stringify({ email, token }));
}

async function submitAuthForm(path, email, password, statusEl) {
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
  } catch (err) {
    setStatus(statusEl, 'Unable to reach the server. Is it running?', true);
  }
}

signupForm?.addEventListener('submit', async (e) => {
  e.preventDefault();
  await submitAuthForm('/auth/signup', signupEmail.value.trim(), signupPassword.value, signupStatus);
});

loginForm?.addEventListener('submit', async (e) => {
  e.preventDefault();
  await submitAuthForm('/auth/login', loginEmail.value.trim(), loginPassword.value, loginStatus);
});
