const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO === true
  && ['localhost', '127.0.0.1', '::1'].includes(window.location.hostname);
const AUTH_REQUEST_TIMEOUT_MS = 15000;
const AUTH_STATUS_TIMEOUT_MS = 8000;

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
let authReadiness = null;
const urlParams = new URLSearchParams(window.location.search);
const pendingInvite = urlParams.get('invite');
const verificationTokenParam = urlParams.get('verify') || urlParams.get('token');
const resetTokenParam = urlParams.get('reset') || urlParams.get('token');

function setStatus(el, message, isError = false) {
  if (!el) return;
  el.textContent = message;
  el.style.color = isError ? '#ffb3b3' : 'var(--muted)';
}

function canonicalEmail(value) {
  return String(value || '').trim().toLowerCase();
}

function authHealthUrl() {
  return `${apiBase.replace(/\/api\/?$/, '')}/api/health`;
}

function authStatusUrl() {
  return `${apiBase}/auth/status`;
}

function requestReference(error) {
  const value = String(error?.requestId || '').trim();
  return value ? ` Reference: ${value}.` : '';
}

function retryAfterMessage(value) {
  const seconds = Number.parseInt(value, 10);
  if (!Number.isFinite(seconds) || seconds <= 0) return 'Try again later.';
  if (seconds < 60) return `Try again in about ${seconds} seconds.`;
  const minutes = Math.ceil(seconds / 60);
  if (minutes < 60) return `Try again in about ${minutes} minute${minutes === 1 ? '' : 's'}.`;
  const hours = Math.ceil(minutes / 60);
  return `Try again in about ${hours} hour${hours === 1 ? '' : 's'}.`;
}

function describeRequestError(error, fallback = 'The request could not be completed.') {
  const reference = requestReference(error);
  if (error?.status === 429) {
    return `Too many attempts. ${retryAfterMessage(error.retryAfter)}${reference}`;
  }
  if (error?.timedOut) {
    return `The request timed out. Check your connection before trying again.${reference}`;
  }
  if (error?.status === 503 || error?.unavailable) {
    return `The authentication service is temporarily unavailable. No local account or session was created.${reference}`;
  }
  return `${error?.data?.error || error?.message || fallback}${reference}`;
}

function beginFormSubmission(form, busyLabel) {
  if (!form || form.dataset.submitting === 'true') return null;
  form.dataset.submitting = 'true';
  form.setAttribute('aria-busy', 'true');
  const button = form.querySelector('button[type="submit"]');
  const originalLabel = button?.textContent || '';
  if (button) {
    button.disabled = true;
    button.textContent = busyLabel;
  }
  return () => {
    delete form.dataset.submitting;
    form.removeAttribute('aria-busy');
    if (button) {
      button.disabled = false;
      button.textContent = originalLabel;
    }
  };
}

async function fetchJson(url, options = {}, timeoutMs = AUTH_REQUEST_TIMEOUT_MS) {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {
      ...options,
      cache: 'no-store',
      credentials: 'omit',
      headers: {
        Accept: 'application/json',
        ...(options.headers || {})
      },
      signal: controller.signal
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      const error = new Error(data?.error || `Request failed with ${response.status}`);
      error.status = response.status;
      error.data = data;
      error.retryAfter = response.headers.get('Retry-After') || '';
      error.requestId = response.headers.get('X-CFF-Request-Id') || '';
      error.unavailable = response.status === 503;
      throw error;
    }
    return {
      data,
      requestId: response.headers.get('X-CFF-Request-Id') || ''
    };
  } catch (error) {
    if (error?.name === 'AbortError') {
      const timeoutError = new Error('Request timed out');
      timeoutError.timedOut = true;
      timeoutError.unavailable = true;
      throw timeoutError;
    }
    if (!error?.status) error.unavailable = true;
    throw error;
  } finally {
    window.clearTimeout(timeout);
  }
}

function describeAuthReadiness(payload = {}) {
  const database = payload.database ? `DB ${payload.database}` : 'DB unknown';
  const verification = payload.emailVerificationRequired ? 'verification on' : 'verification off';
  const email = payload.emailDeliveryConfigured ? 'email ready' : 'email not configured';
  const policy = payload.passwordPolicy?.minLength
    ? `passwords ${payload.passwordPolicy.minLength}-${payload.passwordPolicy.maxLength || 72} chars`
    : '';
  return ['Auth ready', database, verification, email, policy].filter(Boolean).join(' / ');
}

function applyPasswordPolicy(policy = {}) {
  const min = Number(policy.minLength || 12);
  const max = Number(policy.maxLength || 72);
  [signupPassword, resetPassword].forEach((input) => {
    if (!input) return;
    if (Number.isFinite(min) && min > 0) input.minLength = min;
    if (Number.isFinite(max) && max > 0) input.maxLength = max;
  });
  const helper = document.getElementById('signup-password-help');
  if (helper && Number.isFinite(min) && Number.isFinite(max)) {
    helper.textContent = `Use ${min}-${max} characters and avoid common passwords or your email name.`;
  }
}

async function checkAuthApiStatus() {
  if (!authApiStatus) return;
  setStatus(authApiStatus, 'Checking authentication service...');
  try {
    const { data: payload } = await fetchJson(authStatusUrl(), {}, AUTH_STATUS_TIMEOUT_MS);
    authReadiness = payload;
    applyPasswordPolicy(payload.passwordPolicy);
    const degraded = payload.ready === false || payload.status === 'degraded';
    setStatus(authApiStatus, degraded ? (payload.message || describeAuthReadiness(payload)) : describeAuthReadiness(payload), degraded);
  } catch {
    try {
      const { data: payload } = await fetchJson(authHealthUrl(), {}, AUTH_STATUS_TIMEOUT_MS);
      const database = payload.database ? ` / database: ${payload.database}` : '';
      setStatus(authApiStatus, `API: ${payload.status || 'ok'}${database}. Authentication status is temporarily unavailable.`);
    } catch {
      setStatus(authApiStatus, 'Authentication service is currently unreachable. Please try again later.', true);
    }
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
  if (signOutBtn) signOutBtn.hidden = !storedAuth;
}

function createLocalSession(email) {
  return {
    email,
    token: `local-demo-${Date.now().toString(36)}`,
    message: 'Local preview session created'
  };
}

async function postJson(path, body, token = '') {
  const headers = { 'Content-Type': 'application/json' };
  if (token) headers.Authorization = `Bearer ${token}`;
  const { data } = await fetchJson(`${apiBase}${path}`, {
    method: 'POST',
    headers,
    body: JSON.stringify(body || {})
  });
  return data;
}

function redirectToVerification(email, delivered) {
  const targetPage = delivered ? 'verify-email.html' : 'resend-verification.html';
  const next = new URL(targetPage, window.location.href);
  next.searchParams.set('email', email);
  window.setTimeout(() => {
    window.location.href = next.pathname + next.search;
  }, 1100);
}

async function submitAuthForm(path, rawEmail, password, statusEl, redirectTo, form) {
  const finish = beginFormSubmission(form, path === '/auth/signup' ? 'Creating account...' : 'Signing in...');
  if (!finish) return;
  const email = canonicalEmail(rawEmail);
  setStatus(statusEl, path === '/auth/signup' ? 'Creating account...' : 'Signing in...');
  try {
    if (authReadiness?.signupEnabled === false && path === '/auth/signup') {
      setStatus(statusEl, authReadiness.message || 'Signup is temporarily unavailable because authentication storage is not ready.', true);
      return;
    }
    if (authReadiness?.loginEnabled === false && path === '/auth/login') {
      setStatus(statusEl, authReadiness.message || 'Login is temporarily unavailable because authentication storage is not ready.', true);
      return;
    }

    let authenticated = false;
    try {
      const data = await postJson(path, { email, password });
      if (data.token) {
        saveAuth(data.email || email, data.token);
        authenticated = true;
      }

      if (path === '/auth/signup' && data.signupAccepted === true) {
        setStatus(statusEl, data.message || 'Request accepted. Check your email for a verification link, or use Resend verification if it does not arrive.');
        redirectToVerification(email, false);
        return;
      }

      if (path === '/auth/signup' && data.accountMayExist === true) {
        setStatus(statusEl, 'Request accepted. Check your email for a verification link, or use Resend verification if it does not arrive.');
        redirectToVerification(email, false);
        return;
      }

      const verifyRequired = data.emailVerificationRequired && !data.emailVerified;
      const verificationDelivered = data.emailSent === true;
      if (verifyRequired && !authenticated) {
        const message = verificationDelivered
          ? 'Account created. Check your email to verify it before signing in.'
          : 'Account created, but the verification email was not delivered. Use Resend verification to request another message.';
        setStatus(statusEl, message);
        redirectToVerification(email, verificationDelivered);
        return;
      }

      setStatus(statusEl, data.message || 'Success');
      if (authenticated && redirectTo) {
        window.location.href = pendingInvite
          ? `${redirectTo}?invite=${encodeURIComponent(pendingInvite)}`
          : redirectTo;
      }
    } catch (error) {
      if (path === '/auth/signup' && error.status === 409) {
        setStatus(statusEl, 'Request accepted. Check your email for a verification link, or use Resend verification if it does not arrive.');
        redirectToVerification(email, false);
        return;
      }
      if (path === '/auth/signup' && (error.timedOut || (!error.status && error.unavailable))) {
        setStatus(statusEl, `The signup request could not be confirmed. The account may already have been created; use Resend verification or sign in before retrying.${requestReference(error)}`, true);
        return;
      }
      if (error.status) {
        const verifyHint = error.status === 403 && /verification/i.test(error.data?.error || error.message)
          ? ' Use the verification link from your email or request a new one.'
          : '';
        setStatus(statusEl, `${describeRequestError(error)}${verifyHint}`, true);
        return;
      }
      if (!allowLocalDemo) {
        setStatus(statusEl, describeRequestError(error, 'Authentication service unavailable.'), true);
        return;
      }
      const local = createLocalSession(email);
      saveAuth(local.email, local.token);
      setStatus(statusEl, local.message);
      if (redirectTo) window.location.href = redirectTo;
    }
  } finally {
    finish();
  }
}

function stripUrlParams() {
  if (window.location.search) {
    window.history.replaceState({}, document.title, window.location.pathname);
  }
}

signupForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  await submitAuthForm('/auth/signup', signupEmail.value, signupPassword.value, signupStatus, pendingInvite ? 'league.html' : 'signin.html', signupForm);
});

loginForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  await submitAuthForm('/auth/login', loginEmail.value, loginPassword.value, loginStatus, 'league.html', loginForm);
});

verifyForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  const finish = beginFormSubmission(verifyForm, 'Verifying...');
  if (!finish) return;
  setStatus(verifyStatus, 'Verifying...');
  try {
    const data = await postJson('/auth/verify-email', { token: verifyToken.value.trim() });
    setStatus(verifyStatus, data.message || 'Email verified. You can sign in now.');
  } catch (error) {
    setStatus(verifyStatus, describeRequestError(error, 'Could not verify email.'), true);
  } finally {
    finish();
  }
});

resendForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (authReadiness?.emailFlowsEnabled === false) {
    setStatus(resendStatus, 'Verification email cannot be resent until transactional email is configured.', true);
    return;
  }
  const finish = beginFormSubmission(resendForm, 'Sending...');
  if (!finish) return;
  setStatus(resendStatus, 'Sending...');
  try {
    const data = await postJson('/auth/resend-verification', { email: canonicalEmail(resendEmail.value) });
    setStatus(resendStatus, data.message || 'If the account needs verification, an email will be sent.');
  } catch (error) {
    setStatus(resendStatus, describeRequestError(error, 'Could not resend verification.'), true);
  } finally {
    finish();
  }
});

resetRequestForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  if (authReadiness?.emailFlowsEnabled === false) {
    setStatus(resetRequestStatus, 'Password reset email cannot be sent until transactional email is configured.', true);
    return;
  }
  const finish = beginFormSubmission(resetRequestForm, 'Sending...');
  if (!finish) return;
  setStatus(resetRequestStatus, 'Sending...');
  try {
    const data = await postJson('/auth/request-password-reset', { email: canonicalEmail(resetEmail.value) });
    setStatus(resetRequestStatus, data.message || 'If the account exists, a reset email will be sent.');
    if (data.passwordResetToken) {
      const next = new URL('reset-password.html', window.location.href);
      next.searchParams.set('token', data.passwordResetToken);
      window.setTimeout(() => { window.location.href = next.pathname + next.search; }, 600);
    }
  } catch (error) {
    setStatus(resetRequestStatus, describeRequestError(error, 'Could not request password reset.'), true);
  } finally {
    finish();
  }
});

resetCompleteForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  const finish = beginFormSubmission(resetCompleteForm, 'Resetting...');
  if (!finish) return;
  setStatus(resetCompleteStatus, 'Resetting...');
  try {
    const data = await postJson('/auth/reset-password', {
      token: resetToken.value.trim(),
      password: resetPassword.value
    });
    setStatus(resetCompleteStatus, data.message || 'Password reset. Sign in with your new password.');
    resetPassword.value = '';
    window.setTimeout(() => { window.location.href = 'signin.html'; }, 800);
  } catch (error) {
    setStatus(resetCompleteStatus, describeRequestError(error, 'Could not reset password.'), true);
  } finally {
    finish();
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
  const emailParam = canonicalEmail(urlParams.get('email') || '');
  if (resendEmail && emailParam) resendEmail.value = emailParam;
  if (resetEmail && emailParam) resetEmail.value = emailParam;
  if (loginEmail && emailParam) loginEmail.value = emailParam;
  if (resendEmail && signupEmail?.value) resendEmail.value = canonicalEmail(signupEmail.value);
  loadStoredAuth();
  updateAuthUi();
  await validateAuthSession();
  loadStoredAuth();
  updateAuthUi();
  await checkAuthApiStatus();
  stripUrlParams();
}

initAuthPage();
