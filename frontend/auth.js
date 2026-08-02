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
const authApiStatus = document.getElementById('auth-api-status');
const verifyForm = document.getElementById('verify-form');
const verifyToken = document.getElementById('verify-token');
const verifyStatus = document.getElementById('verify-status');
const resendForm = document.getElementById('resend-form');
const resendEmail = document.getElementById('resend-email');
const resendStatus = document.getElementById('resend-status');
const resetRequestForm = document.getElementById('reset-request-form');
const resetEmail = document.getElementById('reset-email');
const resetRequestStatus = document.getElementById('reset-request-status');
const resetCompleteForm = document.getElementById('reset-complete-form');
const resetToken = document.getElementById('reset-token');
const resetPassword = document.getElementById('reset-password');
const resetCompleteStatus = document.getElementById('reset-complete-status');

let storedAuth = null;
const urlParams = new URLSearchParams(window.location.search);
const pendingInvite = urlParams.get('invite');
const verificationTokenParam = urlParams.get('verify') || urlParams.get('token');
const resetTokenParam = urlParams.get('reset') || urlParams.get('token');

function setStatus(el, message, isError = false) {
  if (!el) return;
  el.textContent = message;
  el.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function authHealthUrl() {
  return `${apiBase.replace(/\/api\/?$/, '')}/api/health`;
}

async function checkAuthApiStatus() {
  if (!authApiStatus) return;
  setStatus(authApiStatus, `API: checking ${apiBase}`);
  try {
    const response = await fetch(authHealthUrl(), { headers: { Accept: 'application/json' } });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      setStatus(authApiStatus, `API: ${response.status} from ${apiBase}`, true);
      return;
    }
    const database = payload.database ? ` / database: ${payload.database}` : '';
    setStatus(authApiStatus, `API: ${payload.status || 'ok'}${database}`);
  } catch {
    setStatus(authApiStatus, `API unreachable at ${apiBase}. Check frontend CFF_API_BASE and backend ALLOWED_ORIGINS.`, true);
  }
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

async function postJson(path, body, token = '') {
  const headers = { 'Content-Type': 'application/json' };
  if (token) headers.Authorization = `Bearer ${token}`;
  const resp = await fetch(`${apiBase}${path}`, {
    method: 'POST',
    headers,
    body: JSON.stringify(body || {})
  });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    const error = new Error(data?.error || `Request failed with ${resp.status}`);
    error.status = resp.status;
    error.data = data;
    throw error;
  }
  return data;
}

async function submitAuthForm(path, email, password, statusEl, redirectTo) {
  setStatus(statusEl, 'Working...');
  let authenticated = false;
  try {
    const data = await postJson(path, { email, password });
    if (data.token) {
      saveAuth(data.email || email, data.token);
      authenticated = true;
    }
    const verifyRequired = data.emailVerificationRequired && !data.emailVerified;
    const verificationDelivered = data.emailSent === true;
    const verifyCopy = verifyRequired
      ? verificationDelivered
        ? ' Check your email to verify before signing in.'
        : ' Your account was created, but the verification email could not be delivered. Fix the email provider and then request a new verification email.'
      : '';
    setStatus(statusEl, `${data.message || 'Success'}${verifyCopy}`, verifyRequired && !verificationDelivered);
    if (verifyRequired && !authenticated) {
      const targetPage = verificationDelivered ? 'verify-email.html' : 'resend-verification.html';
      const next = new URL(targetPage, window.location.href);
      next.searchParams.set('email', email);
      setTimeout(() => { window.location.href = next.pathname + next.search; }, 1000);
      return;
    }
  } catch (error) {
    if (error.status) {
      const message = error.data?.error || error.message;
      const verifyHint = error.status === 403 && /verification/i.test(message)
        ? ' Use the verification link from your email or request a new one.'
        : '';
      const existingAccountHint = path === '/auth/signup' && error.status === 409 && /account already exists/i.test(message)
        ? ' A previous signup may already have created this unverified account. Use Resend verification instead of signing up again.'
        : '';
      setStatus(statusEl, `${message}${verifyHint}${existingAccountHint}`, true);
      return;
    }
    if (!allowLocalDemo) {
      setStatus(statusEl, 'API unavailable. Local demo sessions are disabled for this deployment.', true);
      return;
    }
    const local = createLocalSession(email);
    saveAuth(local.email, local.token);
    authenticated = true;
    setStatus(statusEl, local.message);
  }
  if (authenticated && redirectTo) {
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
  await submitAuthForm('/auth/signup', signupEmail.value.trim(), signupPassword.value, signupStatus, pendingInvite ? 'league.html' : 'signin.html');
});

loginForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  await submitAuthForm('/auth/login', loginEmail.value.trim(), loginPassword.value, loginStatus, 'league.html');
});

verifyForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  setStatus(verifyStatus, 'Verifying...');
  try {
    const data = await postJson('/auth/verify-email', { token: verifyToken.value.trim() });
    setStatus(verifyStatus, data.message || 'Email verified. You can sign in now.');
  } catch (error) {
    setStatus(verifyStatus, error.data?.error || error.message || 'Could not verify email.', true);
  }
});

resendForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  setStatus(resendStatus, 'Sending...');
  try {
    const email = resendEmail.value.trim();
    const data = await postJson('/auth/resend-verification', { email });
    setStatus(resendStatus, data.message || 'Verification email queued.');
  } catch (error) {
    setStatus(resendStatus, error.data?.error || error.message || 'Could not resend verification.', true);
  }
});

resetRequestForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  setStatus(resetRequestStatus, 'Sending...');
  try {
    const data = await postJson('/auth/request-password-reset', { email: resetEmail.value.trim() });
    setStatus(resetRequestStatus, data.message || 'If the account exists, a reset email will be sent.');
    if (data.passwordResetToken) {
      const next = new URL('reset-password.html', window.location.href);
      next.searchParams.set('token', data.passwordResetToken);
      setTimeout(() => { window.location.href = next.pathname + next.search; }, 600);
    }
  } catch (error) {
    setStatus(resetRequestStatus, error.data?.error || error.message || 'Could not request password reset.', true);
  }
});

resetCompleteForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  setStatus(resetCompleteStatus, 'Resetting...');
  try {
    const data = await postJson('/auth/reset-password', {
      token: resetToken.value.trim(),
      password: resetPassword.value
    });
    setStatus(resetCompleteStatus, data.message || 'Password reset. Sign in with your new password.');
    resetPassword.value = '';
    setTimeout(() => { window.location.href = 'signin.html'; }, 800);
  } catch (error) {
    setStatus(resetCompleteStatus, error.data?.error || error.message || 'Could not reset password.', true);
  }
});

signOutBtn?.addEventListener('click', async () => {
  const token = storedAuth?.token;
  if (token && !String(token).startsWith('local-demo-')) {
    try {
      await postJson('/auth/logout', {}, token);
    } catch {
      // Local session cleanup still happens below.
    }
  }
  clearAuth();
  setStatus(loginStatus, 'Signed out');
});

async function initAuthPage() {
  if (verifyToken && verificationTokenParam) verifyToken.value = verificationTokenParam;
  if (resetToken && resetTokenParam) resetToken.value = resetTokenParam;
  const emailParam = urlParams.get('email') || '';
  if (resendEmail && emailParam) resendEmail.value = emailParam;
  if (resetEmail && emailParam) resetEmail.value = emailParam;
  if (loginEmail && emailParam) loginEmail.value = emailParam;
  if (resendEmail && signupEmail?.value) resendEmail.value = signupEmail.value;
  loadStoredAuth();
  updateAuthUi();
  await validateAuthSession();
  loadStoredAuth();
  updateAuthUi();
  await checkAuthApiStatus();
  stripUrlParams();
}

initAuthPage();
